#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>
#include <TodoList.h>

/**
 * Persists the to-do list at /.crosspoint/todo.json.
 *
 * Unlike the other stores this one is NOT kept in RAM permanently: TodoActivity calls
 * load() in onEnter() and saveIfDirty() + unload() in onExit(), so the list (about
 * 21 KB at the 100-item / 120-byte limits: 4.8 KB of records, ~13 KB of text blocks, 3 KB of tombstones) only occupies
 * heap while the to-do screen is open.
 *
 * All file access goes through the SD card, which shares SPI with the display, so the
 * caller must hold the RenderLock for load(), saveIfDirty() and anything that mutates
 * the list (the render task reads it).
 */
class TodoStore : public PersistableStore<TodoStore> {
 private:
  TodoList todoList;
  bool loaded = false;
  bool dirty = false;

  TodoStore();
  ~TodoStore() = default;

  friend class PersistableStore<TodoStore>;

 public:
  static constexpr int FORMAT_VERSION = 1;

  static const char* getFilePath() { return "/.crosspoint/todo.json"; }
  static const char* getBackupPath() { return "/.crosspoint/todo.json.bak"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Parses a phone's request body (a C1 document plus "baseRev") into `out`. Checks shape only: version 1,
  // objects, ids, a well-formed baseRev, and the array-size limits (which bound the allocation, so they run
  // here, before the baseRev conflict check). So a request with a stale baseRev AND an oversized array, a bad
  // id or a bad baseRev gets this 400, not the 409 that applyIncoming would give: an app must not depend on a
  // 409 in those cases. Text is sanitized as it is copied, so no raw text is kept.
  // Everything else is TodoList::applyIncoming()'s job (docs/todo-sync-protocol.md). On failure `error`
  // points to a static reason and `out` is emptied. It touches no shared state, so it may run without the
  // RenderLock; responses reuse toJson().
  //
  // Memory plan for the transports (the list is ~21 KB at its limits, `out` about as much): size the
  // request buffer from the announced length and refuse with "busy/out of memory" when it does not fit;
  // free the request buffer and the JsonDocument before calling applyIncoming(); build `out` in a tight
  // scope and destroy it before serializing the response; log free heap and largest block around the
  // transfer, with the Bluetooth stack up, before fixing any limit.
  static bool parseIncoming(JsonVariantConst body, TodoIncoming& out, const char*& error);

  // Loads the list. A missing file yields an empty list. An unreadable or newer-format
  // file is first copied aside to todo.json.bak so a later save can't destroy it.
  void load();
  // Writes the file if there are unsaved edits. Returns false only on a write failure.
  // The caller must hold the RenderLock: toJson() stores item text by reference, so the list must not
  // change until the file is written (any future sync task must take the same lock).
  bool saveIfDirty();
  // Releases the in-memory list. Unsaved edits are dropped, so call saveIfDirty() first.
  void unload();

  bool isLoaded() const { return loaded; }
  bool isDirty() const { return dirty; }
  void markDirty() { dirty = true; }

  const TodoList& list() const { return todoList; }
  TodoList& list() { return todoList; }
};

// Helper macro to access the to-do store
#define TODO_STORE TodoStore::getInstance()
