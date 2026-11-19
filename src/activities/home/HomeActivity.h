#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
 public:
  static constexpr int kCarouselFrameCount = 3;

 private:
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int lastCarouselBookIndex = 0;  // remembered position when leaving the carousel row
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  // The home "library" slot (index 2) shows Plugins while the plugin system is
  // enabled, otherwise OPDS when a server is configured. NONE hides the slot.
  // NetworkModeSelectionActivity lists OPDS in exactly the other cases.
  HomeMenuItem slotItem() const {
    if (hasPlugins) return HomeMenuItem::PLUGINS;
    return hasOpdsServers ? HomeMenuItem::OPDS_BROWSER : HomeMenuItem::NONE;
  }
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Output polarity the snapshot was taken in (night mode stores images pre-inverted).
  bool coverBufferInverted = false;
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;

  // Aliases into the static carousel frame cache (see HomeActivity.cpp)
  uint8_t* carouselFrames[kCarouselFrameCount] = {nullptr, nullptr, nullptr};
  bool carouselFramesReady = false;

  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // The icon-menu entries that are visible, in display order. Rebuilt in onEnter() from the Home screen
  // settings; Settings is always last. Every count, label and index lookup derives from this one list.
  static constexpr int MAX_MENU_ENTRIES = 6;  // Browse, Library, Plugins/OPDS, To-do, File Transfer, Settings
  // The Home screen settings are read once, in onEnter(): they can't change while Home is on screen (the web
  // settings page only runs from the File Transfer screen), and Home is rebuilt on every return.
  HomeMenuItem menuEntries[MAX_MENU_ENTRIES] = {};
  int menuEntryCount = 0;
  void rebuildMenuEntries();
  // Menu index of an item (used in onEnter); 0 when it isn't shown.
  int menuIndexOf(HomeMenuItem item) const;
  HomeMenuItem menuItemAt(int index) const;  // NONE when out of range (used in loop)
  void onRecentBookSelected(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();
  void onTodoOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  // Standard icon-menu labels/icons (Browse, Library, optional Plugins/OPDS slot, Transfer, Settings).
  void appendHomeMenu(std::vector<const char*>& items, std::vector<UIIcon>& icons) const;
  void preRenderCarouselFrames();
  void freeCarouselFrames();
  void renderCarouselFrame(int bookIdx, int slotIdx);
  void updateSlidingWindowCache(int centerIdx, int bookCount);
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
