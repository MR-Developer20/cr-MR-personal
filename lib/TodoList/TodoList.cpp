#include "TodoList.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {
constexpr bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }
constexpr bool isBlank(unsigned char c) { return c == ' ' || c < 0x20 || c == 0x7F; }

bool displayOrderLess(const TodoItem& a, const TodoItem& b) {
  if (a.order != b.order) return a.order < b.order;
  return a.id < b.id;  // Deterministic tie-break when merged orders collide.
}
}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string TodoList::sanitizeText(std::string_view in) {
  // Collapse runs of whitespace/control characters into one space and trim both ends,
  // so stray newlines from the keyboard or a phone can't break a list row.
  std::string out;
  out.reserve(std::min(in.size(), MAX_TEXT_BYTES + 1));
  bool pendingSpace = false;
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    if (isBlank(c)) {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(ch);
    if (out.size() > MAX_TEXT_BYTES) break;  // One byte past the limit is enough to cut safely.
  }

  if (out.size() > MAX_TEXT_BYTES) {
    size_t cut = MAX_TEXT_BYTES;
    // Back up so the cut never lands inside a multi-byte UTF-8 sequence.
    while (cut > 0 && isUtf8Continuation(static_cast<unsigned char>(out[cut]))) --cut;
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

void TodoList::idToHex(const uint64_t id, char out[17]) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    out[i] = kDigits[(id >> ((15 - i) * 4)) & 0xF];
  }
  out[16] = '\0';
}

bool TodoList::hexToId(const std::string_view hex, uint64_t& out) {
  if (hex.size() != 16) return false;
  uint64_t value = 0;
  for (const char c : hex) {
    uint64_t nibble;
    if (c >= '0' && c <= '9') {
      nibble = static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      nibble = static_cast<uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | nibble;
  }
  if (value == 0) return false;
  out = value;
  return true;
}

size_t TodoList::doneCount() const {
  return static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const TodoItem& i) { return i.done; }));
}

void TodoList::touch(TodoItem& item) {
  ++item.rev;
  item.seq = ++listRev;
}

bool TodoList::idInUse(const uint64_t id) const {
  const auto matches = [id](const TodoItem& i) { return i.id == id; };
  return std::any_of(items.begin(), items.end(), matches) || std::any_of(deleted.begin(), deleted.end(), matches);
}

uint64_t TodoList::newId() const {
  uint64_t id = 0;
  for (int attempt = 0; attempt < 8; ++attempt) {
    id = (static_cast<uint64_t>(rng()) << 32) | rng();
    if (id != 0 && !idInUse(id)) return id;
  }
  // A broken RNG must not stall the UI: walk forward to the next free id.
  do {
    ++id;
  } while (id == 0 || idInUse(id));
  return id;
}

void TodoList::addTombstone(const TodoItem& from) {
  TodoItem tombstone;
  tombstone.id = from.id;
  tombstone.rev = from.rev + 1;
  tombstone.seq = ++listRev;
  deleted.push_back(std::move(tombstone));
  if (deleted.size() > MAX_TOMBSTONES) {
    deleted.erase(deleted.begin());  // Oldest first; a phone that missed it re-sends nothing harmful.
  }
}

void TodoList::renumberOrders() {
  int32_t next = ORDER_STEP;
  for (auto& item : items) {
    if (item.order != next) {
      item.order = next;
      touch(item);
    }
    next += ORDER_STEP;
  }
}

// ---------------------------------------------------------------------------
// Local edits
// ---------------------------------------------------------------------------

TodoEditResult TodoList::add(const std::string_view text) {
  std::string clean = sanitizeText(text);
  if (clean.empty()) return TodoEditResult::EmptyText;
  if (isFull()) return TodoEditResult::ListFull;

  if (!items.empty() && items.back().order > std::numeric_limits<int32_t>::max() - ORDER_STEP) {
    renumberOrders();
  }

  TodoItem item;
  item.id = newId();
  item.text = std::move(clean);
  item.order = items.empty() ? ORDER_STEP : items.back().order + ORDER_STEP;
  touch(item);
  items.push_back(std::move(item));
  return TodoEditResult::Ok;
}

TodoEditResult TodoList::setText(const size_t index, const std::string_view text) {
  if (index >= items.size()) return TodoEditResult::BadIndex;
  std::string clean = sanitizeText(text);
  if (clean.empty()) return TodoEditResult::EmptyText;
  if (clean == items[index].text) return TodoEditResult::NoChange;
  items[index].text = std::move(clean);
  touch(items[index]);
  return TodoEditResult::Ok;
}

TodoEditResult TodoList::toggleDone(const size_t index) {
  if (index >= items.size()) return TodoEditResult::BadIndex;
  items[index].done = !items[index].done;
  touch(items[index]);
  return TodoEditResult::Ok;
}

TodoEditResult TodoList::remove(const size_t index) {
  if (index >= items.size()) return TodoEditResult::BadIndex;
  addTombstone(items[index]);
  items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
  return TodoEditResult::Ok;
}

TodoEditResult TodoList::swapWithNext(const size_t index) {
  if (index + 1 >= items.size()) return TodoEditResult::BadIndex;
  if (items[index].order == items[index + 1].order) {
    renumberOrders();  // Equal keys (possible after a merge) can't express the swap.
  }
  std::swap(items[index].order, items[index + 1].order);
  std::swap(items[index], items[index + 1]);
  touch(items[index]);
  touch(items[index + 1]);
  return TodoEditResult::Ok;
}

TodoEditResult TodoList::moveUp(const size_t index) {
  if (index >= items.size()) return TodoEditResult::BadIndex;
  if (index == 0) return TodoEditResult::NoChange;
  return swapWithNext(index - 1);
}

TodoEditResult TodoList::moveDown(const size_t index) {
  if (index >= items.size()) return TodoEditResult::BadIndex;
  if (index + 1 == items.size()) return TodoEditResult::NoChange;
  return swapWithNext(index);
}

size_t TodoList::removeDone() {
  size_t removed = 0;
  for (const auto& item : items) {
    if (item.done) {
      addTombstone(item);
      ++removed;
    }
  }
  if (removed > 0) {
    items.erase(std::remove_if(items.begin(), items.end(), [](const TodoItem& i) { return i.done; }), items.end());
  }
  return removed;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void TodoList::clear() {
  std::vector<TodoItem>().swap(items);  // swap, not clear(): actually returns the heap block
  std::vector<TodoItem>().swap(deleted);
  listRev = 0;
}

void TodoList::reserveCapacity() {
  items.reserve(MAX_ITEMS);
  deleted.reserve(MAX_TOMBSTONES + 1);  // add() trims after the push, so one extra slot is needed
}

void TodoList::beginLoad(const uint32_t revision, const size_t liveHint, const size_t tombstoneHint) {
  clear();
  listRev = revision;
  items.reserve(std::min(liveHint, MAX_ITEMS));
  deleted.reserve(std::min(tombstoneHint, MAX_TOMBSTONES));
}

bool TodoList::loadItem(TodoItem item) {
  if (item.id == 0 || idInUse(item.id) || isFull()) return false;
  item.text = sanitizeText(item.text);
  if (item.text.empty()) return false;
  listRev = std::max(listRev, item.seq);
  items.push_back(std::move(item));
  return true;
}

bool TodoList::loadTombstone(TodoItem item) {
  if (item.id == 0 || idInUse(item.id)) return false;
  item.text.clear();
  item.done = false;
  listRev = std::max(listRev, item.seq);
  deleted.push_back(std::move(item));
  return true;
}

void TodoList::endLoad() {
  std::stable_sort(items.begin(), items.end(), displayOrderLess);
  std::stable_sort(deleted.begin(), deleted.end(), [](const TodoItem& a, const TodoItem& b) { return a.seq < b.seq; });
  if (deleted.size() > MAX_TOMBSTONES) {
    deleted.erase(deleted.begin(), deleted.end() - static_cast<std::ptrdiff_t>(MAX_TOMBSTONES));
  }
}
