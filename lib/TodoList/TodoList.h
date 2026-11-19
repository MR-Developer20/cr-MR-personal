#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * One to-do entry. Live items and deletion markers ("tombstones") share this struct;
 * a tombstone keeps only id/rev/seq so a later phone sync can propagate the delete.
 */
struct TodoItem {
  uint64_t id = 0;    // Random, non-zero, unique. Serialized as 16 lowercase hex chars.
  std::string text;   // Sanitized UTF-8, at most TodoList::MAX_TEXT_BYTES. Empty for tombstones.
  uint32_t rev = 0;   // Per-item revision, +1 on every local change. Merge key for sync.
  uint32_t seq = 0;   // List revision at this item's last local change (local ordering only).
  int32_t order = 0;  // Ascending display position. Synced so reorders propagate.
  bool done = false;
};

enum class TodoEditResult : uint8_t { Ok, NoChange, EmptyText, ListFull, BadIndex };

/**
 * A phone's proposed state, already parsed (docs/todo-sync-protocol.md, C5). `items` are live
 * records; `deleted` are tombstones (only id and rev are used).
 */
struct TodoIncoming {
  uint32_t baseRev = 0;
  std::vector<TodoItem> items;
  std::vector<TodoItem> deleted;
};

enum class TodoApplyStatus : uint8_t {
  Applied,    // List replaced; caller saves and answers 200 with the stored document.
  Unchanged,  // Incoming equals the stored records; nothing changed (idempotent retry).
  Conflict,   // baseRev is stale; caller answers 409 with the current document.
  Invalid,    // Rejected before any change; caller answers 400 with `reason`.
};

struct TodoApplyResult {
  TodoApplyStatus status = TodoApplyStatus::Invalid;
  const char* reason = "";  // Static string; empty unless status is Invalid.
};

/**
 * Platform-independent to-do list model: no Arduino, SD or JSON dependencies, so it is
 * unit-tested on the host (test/todo_list). Persistence lives in src/TodoStore.
 *
 * Indices passed to the edit functions refer to live items in display order.
 * Every successful edit bumps the touched items' `rev` and the list revision, which is
 * what a future phone sync compares; device wall-clock time is never used.
 */
class TodoList {
 public:
  static constexpr size_t MAX_ITEMS = 100;
  static constexpr size_t MAX_TEXT_BYTES = 120;
  static constexpr size_t MAX_TOMBSTONES = 64;
  static constexpr int32_t ORDER_STEP = 1024;

  using RandomFn = uint32_t (*)();
  explicit TodoList(RandomFn rng) : rng(rng) {}

  // --- Queries ---
  size_t size() const { return items.size(); }
  bool empty() const { return items.empty(); }
  bool isFull() const { return items.size() >= MAX_ITEMS; }
  const TodoItem& at(size_t index) const { return items[index]; }
  size_t doneCount() const;
  uint32_t revision() const { return listRev; }
  const std::vector<TodoItem>& liveItems() const { return items; }
  const std::vector<TodoItem>& tombstones() const { return deleted; }

  // --- Local edits ---
  TodoEditResult add(std::string_view text);
  TodoEditResult setText(size_t index, std::string_view text);
  TodoEditResult toggleDone(size_t index);
  TodoEditResult remove(size_t index);
  TodoEditResult moveUp(size_t index);
  TodoEditResult moveDown(size_t index);
  size_t removeDone();  // Returns the number of items removed.

  // --- Sync ---
  // Applies a phone's state exactly as C5 steps 2-6 describe. All-or-nothing: on Conflict and
  // Invalid the list is untouched. The text of `incoming` is sanitized in place and its records
  // are moved into the list, so applyIncoming() itself builds no second copy of the list.
  //
  // Caller contract (this class knows nothing about locks, files or the dirty flag):
  //  - the list must be loaded (TodoStore::load() reserves capacity, so the swap-in never reallocates);
  //  - hold the RenderLock from this call until the response is serialized and the list is saved: the
  //    render task reads the list, and TodoStore::toJson() stores text by reference;
  //  - only on Applied: mark the store dirty. Unchanged, Conflict and Invalid must not write anything.
  TodoApplyResult applyIncoming(TodoIncoming&& incoming);

  // --- Loading persisted state (no revision bumps) ---
  void clear();  // Empties the list and releases its heap memory.
  // Reserves room for MAX_ITEMS live items and MAX_TOMBSTONES + 1 tombstones (about 8 KB) in one
  // allocation each, so edits never regrow the vectors. clear() releases it again.
  void reserveCapacity();
  void beginLoad(uint32_t revision, size_t liveHint, size_t tombstoneHint);
  bool loadItem(TodoItem item);       // False if rejected: bad/duplicate id, empty text, list full.
  bool loadTombstone(TodoItem item);  // False if rejected: bad/duplicate id.
  void endLoad();                     // Sorts by order and trims excess tombstones.

  // --- Helpers (public for tests and the store) ---
  static std::string sanitizeText(std::string_view in);
  static void idToHex(uint64_t id, char out[17]);
  static bool hexToId(std::string_view hex, uint64_t& out);

 private:
  std::vector<TodoItem> items;    // Live items, display order.
  std::vector<TodoItem> deleted;  // Tombstones, oldest (lowest seq) first.
  uint32_t listRev = 0;
  RandomFn rng;

  void touch(TodoItem& item);
  uint64_t newId() const;
  bool idInUse(uint64_t id) const;
  void addTombstone(const TodoItem& from);
  TodoEditResult swapWithNext(size_t index);
  void renumberOrders();
};
