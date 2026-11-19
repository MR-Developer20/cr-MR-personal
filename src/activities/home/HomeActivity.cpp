#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Static carousel frame cache — survives HomeActivity re-creation so that
// returning to home (e.g. after settings) doesn't re-read covers from SD.
// Freed in onExit() and again in onRecentBookSelected() before entering the reader.
// ---------------------------------------------------------------------------
namespace {
uint8_t* gCachedFrames[HomeActivity::kCarouselFrameCount] = {};
int gCachedFrameBookIdx[HomeActivity::kCarouselFrameCount] = {-1, -1, -1};
// Night mode keeps images pre-inverted in the framebuffer, so frames baked in one output
// polarity show covers as negatives in the other.
bool gCachedFramesInverted = false;
int gCachedFrameCount = 0;
std::string gCacheKey;

int findFrameSlot(int bookIdx) {
  for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
    if (gCachedFrameBookIdx[i] == bookIdx && gCachedFrames[i] != nullptr) return i;
  }
  return -1;
}

void invalidateCarouselCache() {
  for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
    if (gCachedFrames[i]) {
      free(gCachedFrames[i]);
      gCachedFrames[i] = nullptr;
    }
    gCachedFrameBookIdx[i] = -1;
  }
  gCachedFrameCount = 0;
  gCacheKey.clear();
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Library, File transfer, Settings
  if (hasTodoEntry()) count++;
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (slotItem() != HomeMenuItem::NONE) {
    count++;
  }
  return count;
}

void HomeActivity::appendHomeMenu(std::vector<const char*>& items, std::vector<UIIcon>& icons) const {
  items = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE)};
  icons = {Folder, Library, Transfer, Settings};
  if (slotItem() != HomeMenuItem::NONE) {
    items.insert(items.begin() + 2, hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
    icons.insert(icons.begin() + 2, hasPlugins ? Plugins : Book);
  }
  if (hasTodoEntry()) {
    const int todoAt = slotItem() != HomeMenuItem::NONE ? 3 : 2;
    items.insert(items.begin() + todoAt, tr(STR_TODO_TITLE));
    icons.insert(icons.begin() + todoAt, Todo);
  }
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  // Tracks which book indices had a carousel thumbnail generated this pass. Only the carousel
  // branch writes it, and recentBooks holds at most the active theme's homeRecentBooksCount.
  constexpr int kCarouselBooks = LyraCarouselMetrics::values.homeRecentBooksCount;
  bool bookUpdated[kCarouselBooks] = {};
  Rect popupRect;

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty() && isCarouselTheme) {
      // Carousel: exact-size thumbnails for the centre and side slots.
      const std::string centerPath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterCoverW,
                                                                LyraCarouselTheme::kCenterCoverH);
      const std::string sidePath =
          UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
      const bool centerMissing = !Storage.exists(centerPath.c_str());
      const bool sideMissing = !Storage.exists(sidePath.c_str());

      if (centerMissing || sideMissing) {
        // Runs `generate(w, h)` for whichever of the two slot sizes is missing.
        auto generateMissing = [&](auto&& generate) {
          bool ok = true;
          if (centerMissing) ok = generate(LyraCarouselTheme::kCenterCoverW, LyraCarouselTheme::kCenterCoverH) && ok;
          if (sideMissing) ok = generate(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && ok;
          return ok;
        };
        auto beginPopup = [&]() {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
        };
        auto finishBook = [&](bool success) {
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          } else if (progress < kCarouselBooks) {
            bookUpdated[progress] = true;
          }
          coverRendered = false;
          requestUpdate();
        };

        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          const bool cacheLoaded = epub.load(false, true);
          beginPopup();
          // Without a book cache (e.g. after "Delete Book Cache") read the cover location
          // straight from the EPUB.
          finishBook(generateMissing([&](int w, int h) {
            return cacheLoaded ? epub.generateThumbBmp(w, h) : epub.generateThumbBmpFromSource(w, h);
          }));
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            beginPopup();
            finishBook(generateMissing([&](int w, int h) { return xtc.generateThumbBmp(w, h); }));
          }
        }
      }
    } else if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub/txt/md, try to load the metadata for title/author and cover
        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          const bool cacheLoaded = epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          // Without a book cache (e.g. after "Delete Book Cache") there is no cached metadata,
          // so read the cover location straight from the EPUB.
          bool success =
              cacheLoaded ? epub.generateThumbBmp(thumbHeight) : epub.generateThumbBmpFromSource(thumbHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  // Re-render only the affected carousel slots rather than rebuilding the cache.
  if (isCarouselTheme) {
    bool anyUpdated = false;
    for (int i = 0; i < static_cast<int>(recentBooks.size()) && i < kCarouselBooks; ++i) {
      if (!bookUpdated[i]) continue;
      anyUpdated = true;
      if (carouselFramesReady) {
        // Only re-render the slot holding this book; books outside the window
        // will be picked up by updateSlidingWindowCache on next navigation.
        const int slot = findFrameSlot(i);
        if (slot >= 0) renderCarouselFrame(i, slot);
      }
    }
    if (anyUpdated) {
      if (!carouselFramesReady) {
        // Cache not yet initialised (e.g. earlier OOM) — retry the full pre-render.
        preRenderCarouselFrames();
      }
      requestUpdate();
    }
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  // Plugin System is the global opt-in. Once enabled, expose the Plugins entry
  // even when no SD plugins are installed yet so Plugin Hub can be installed
  // from inside the Plugins screen (or users can sideload plugins manually).
  hasPlugins = SETTINGS.pluginsEnabled != 0;
  carouselFramesReady = false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    const HomeMenuItem slot = slotItem();
    coverGridUi->begin(recentBooks,
                       slot == HomeMenuItem::PLUGINS        ? CoverGridHomeUi::SlotTab::PLUGINS
                       : slot == HomeMenuItem::OPDS_BROWSER ? CoverGridHomeUi::SlotTab::OPDS
                                                            : CoverGridHomeUi::SlotTab::NONE,
                       hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex =
      initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, slotItem(), hasTodoEntry());

  // Pre-render carousel frames before the first display update so the fast
  // path is active from render #1. Cache hit = instant. Cache miss = SD reads
  // here, but the E-ink is still refreshing from the previous activity anyway.
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
    preRenderCarouselFrames();
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
  invalidateCarouselCache();
  freeCarouselFrames();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  coverBufferInverted = display.isInverted();
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::freeCarouselFrames() {
  // Instance pointers are aliases into the static cache — do not free here.
  for (int i = 0; i < kCarouselFrameCount; ++i) carouselFrames[i] = nullptr;
  carouselFramesReady = false;
}

void HomeActivity::preRenderCarouselFrames() {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) return;

  // Build cache key from book paths in order
  std::string newKey;
  newKey.reserve(128);
  for (const auto& b : recentBooks) {
    newKey += b.path;
    newKey += '\0';
  }

  // Cache hit: same books in same order — reuse without any SD reads
  if (newKey == gCacheKey && gCachedFrameCount > 0 && gCachedFramesInverted == display.isInverted()) {
    for (int i = 0; i < gCachedFrameCount; ++i) carouselFrames[i] = gCachedFrames[i];
    carouselFramesReady = true;
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  // Cache miss: free old cache and re-render
  invalidateCarouselCache();

  if (!renderer.getFrameBuffer()) return;

  const size_t bufferSize = renderer.getBufferSize();
  freeCoverBuffer();  // release the cover-tile snapshot before allocating frames

  const int frameCount = std::min(bookCount, kCarouselFrameCount);
  for (int i = 0; i < frameCount; ++i) {
    gCachedFrames[i] = static_cast<uint8_t*>(malloc(bufferSize));
    if (!gCachedFrames[i]) {
      LOG_ERR("HOME", "preRenderCarouselFrames: OOM for frame %d (%u bytes)", i, (unsigned)bufferSize);
      invalidateCarouselCache();
      return;  // carouselFramesReady stays false — render() falls back to the slow path
    }
  }

  // Render only the currently-selected cover. Adjacent frames are populated
  // lazily by updateSlidingWindowCache() after the first paint completes,
  // so the user sees their book immediately and scroll latency only appears
  // once they actually navigate.
  const int selectedBookIdx = (selectorIndex < bookCount) ? selectorIndex : lastCarouselBookIndex;
  const int initialBookIdx = (selectedBookIdx >= 0 && selectedBookIdx < bookCount) ? selectedBookIdx : 0;
  renderCarouselFrame(initialBookIdx, 0);

  gCachedFrameCount = frameCount;
  gCacheKey = newKey;
  gCachedFramesInverted = display.isInverted();
  carouselFramesReady = true;
  coverRendered = false;
  coverBufferStored = false;
}

void HomeActivity::renderCarouselFrame(int bookIdx, int slotIdx) {
  const uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || !gCachedFrames[slotIdx]) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int bookCount = static_cast<int>(recentBooks.size());
  bool dummy1 = false, dummy2 = false, dummy3 = false;

  // selectorIndex = bookCount → drawRecentBookCover centres on the pre-render
  // index set below and draws no selection border (baked frames stay neutral).
  LyraCarouselTheme::setPreRenderIndex(bookIdx);
  renderer.clearScreen();
  // The header (clock, battery) is drawn live in render(); baking it would freeze both.
  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, bookCount, dummy1, dummy2, dummy3, []() { return true; });

  memcpy(gCachedFrames[slotIdx], frameBuffer, renderer.getBufferSize());
  gCachedFrameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCachedFrames[slotIdx];
}

void HomeActivity::updateSlidingWindowCache(int centerIdx, int bookCount) {
  // No sliding needed when all books already fit in the cache.
  if (bookCount <= kCarouselFrameCount || !carouselFramesReady) return;

  const int prevIdx = (centerIdx + bookCount - 1) % bookCount;
  const int nextIdx = (centerIdx + 1) % bookCount;

  const bool hasPrev = findFrameSlot(prevIdx) >= 0;
  const bool hasNext = findFrameSlot(nextIdx) >= 0;
  if (hasPrev && hasNext) return;  // window already complete

  const int missingIdx = !hasPrev ? prevIdx : nextIdx;

  // Evict the slot whose book is furthest from centerIdx (never evict the centre
  // itself, nor an adjacent frame that is already cached).
  int evictSlot = -1;
  int maxDist = -1;
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (!gCachedFrames[i]) continue;
    const int bookInSlot = gCachedFrameBookIdx[i];
    if (bookInSlot == centerIdx) continue;
    if (hasPrev && bookInSlot == prevIdx) continue;
    if (hasNext && bookInSlot == nextIdx) continue;
    const int diff = std::abs(bookInSlot - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }

  if (evictSlot >= 0) {
    LOG_DBG("HOME", "carousel: evict slot %d (book %d) -> book %d", evictSlot, gCachedFrameBookIdx[evictSlot],
            missingIdx);
    renderCarouselFrame(missingIdx, evictSlot);
  }
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onRecentBookSelected(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, slotItem(), hasTodoEntry())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::PLUGINS:
        onPluginsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::TODO:
        onTodoOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  // Carousel: two-axis navigation. Left/Right cycles within the focused row; Up/Down (buttons or
  // swipes) toggles between the carousel row and the icon menu row.
  const int carouselBookCount = static_cast<int>(recentBooks.size());
  const auto toggleCarouselRow = [this, carouselBookCount] {
    if (carouselBookCount == 0) return;
    if (selectorIndex < carouselBookCount) {
      lastCarouselBookIndex = selectorIndex;
      selectorIndex = carouselBookCount;
    } else {
      selectorIndex = lastCarouselBookIndex;
    }
    requestUpdate();
  };
  if (isCarousel) {
    const int menuItemCount = menuCount - carouselBookCount;
    const bool inCarouselRow = selectorIndex < carouselBookCount;
    const int menuIdx = inCarouselRow ? 0 : selectorIndex - carouselBookCount;
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (inCarouselRow && carouselBookCount > 0) {
        selectorIndex = (selectorIndex + 1) % carouselBookCount;
      } else if (!inCarouselRow) {
        selectorIndex = carouselBookCount + (menuIdx + 1) % menuItemCount;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (inCarouselRow && carouselBookCount > 0) {
        selectorIndex = (selectorIndex + carouselBookCount - 1) % carouselBookCount;
      } else if (!inCarouselRow) {
        selectorIndex = carouselBookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      toggleCarouselRow();
    }
  }

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  if (!coverGridUi && !isCarousel) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  const auto swipe = mappedInput.wasSwipe();
  if (isCarousel && (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
    toggleCarouselRow();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onRecentBookSelected(recentBooks[0].path);
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
    return;
  }

  if (isCarousel) {
    // The carousel's centre/side slots and tile row don't match the column/row hit grids below.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
    }
    return;
  }

  // Match the rendered cover grid, which lays tiles inside getContentArea()
  // (origin content.x + contentSidePadding, span content.width). Using
  // screen-relative getScreenWidth()/contentSidePadding here offset the touch
  // targets from the drawn covers by content.x on bezel-inset panels (EEGO A4).
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, content.x + metrics.contentSidePadding, coverColumnWidth,
                                               recentCount, metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // Content columns clear the bezel insets on rounded panels. The header
  // self-insets in drawHeader, so only the cover/menu need it here.
  const Rect content = UITheme::getInstance().getContentArea(renderer);

  // Carousel fast path: pre-rendered frame ready — memcpy + border overlay
  // instead of re-reading covers from SD on every selection change.
  if (carouselFramesReady && gCachedFramesInverted != display.isInverted()) {
    // Output polarity flipped since the frames were baked: rebuild them.
    invalidateCarouselCache();
    freeCarouselFrames();
    preRenderCarouselFrames();
  }
  if (carouselFramesReady) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int centerIdx = inCarouselRow ? selectorIndex : lastCarouselBookIndex;
    const int slotIdx = findFrameSlot(centerIdx);

    if (frameBuffer && slotIdx >= 0 && carouselFrames[slotIdx]) {
      memcpy(frameBuffer, carouselFrames[slotIdx], renderer.getBufferSize());

      // Home is the stack root: no back button. Drawn live so the clock and battery stay current.
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                     nullptr, nullptr, false);

      GUI.drawCarouselBorder(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                             inCarouselRow);

      std::vector<const char*> menuItems;
      std::vector<UIIcon> menuIcons;
      menuItems.reserve(7);
      menuIcons.reserve(7);
      appendHomeMenu(menuItems, menuIcons);

      GUI.drawButtonMenu(
          renderer,
          Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
               pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                             metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
          static_cast<int>(menuItems.size()), selectorIndex - static_cast<int>(recentBooks.size()),
          [&menuItems](int index) { return std::string(menuItems[index]); },
          [&menuIcons](int index) { return menuIcons[index]; });

      const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                                tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

      renderer.displayBuffer();
      // E-ink refresh complete — pre-render the missing adjacent frame while idle.
      updateSlidingWindowCache(centerIdx, bookCount);
      // Mirror the slow-path trigger: generate missing thumbnails on the second
      // render so the E-ink is already showing something before the SD work starts.
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      } else if (!recentsLoaded && !recentsLoading) {
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
      }
      return;
    }
  }

  renderer.clearScreen();
  if (coverGridUi) {
    const auto drawGrid = [&] {
      renderer.clearScreen();
      coverGridUi->setSelection(selectorIndex);
      UITheme::getInstance().drawCoverGridHome(*coverGridUi);
      // Front Left/Right walk the tabs, so their hints read Left/Right; the
      // side page buttons (unhinted) walk the covers.
      const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                                tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    };
    drawGrid();
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Resolve the covers before the
    // panel is touched and redraw into the framebuffer, so Home paints once
    // instead of three e-ink refreshes (placeholder, placeholder, covers).
    if (coverGridUi->takeThumbHeightChanged()) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      drawGrid();
      if (coverGridUi->takeThumbHeightChanged()) {
        coverGridUi->refreshCoverPaths();
        recentsLoaded = false;
        requestUpdate();
      }
    }
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    firstRenderDone = true;
    return;
  }
  // A cover snapshot from the other output polarity holds the other byte form (night mode keeps images pre-inverted so
  // they stay positive): pasting it back after a theme switch showed the cover as a negative. Redraw it instead.
  if (coverBufferStored && coverBufferInverted != display.isInverted()) {
    freeCoverBuffer();
    coverRendered = false;
  }
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = content.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = content.width;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems;
  std::vector<UIIcon> menuIcons;
  menuItems.reserve(7);
  menuIcons.reserve(7);
  appendHomeMenu(menuItems, menuIcons);

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, content.width,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const bool carouselHints =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                            carouselHints ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP),
                                            carouselHints ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  firstRenderDone = true;
  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onRecentBookSelected(const std::string& path) {
  // Release the frame cache (up to 3 x 48 KB) before the reader starts allocating.
  invalidateCarouselCache();
  freeCarouselFrames();
  activityManager.goToReader(path);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(); }

void HomeActivity::onTodoOpen() { activityManager.goToTodo(); }
