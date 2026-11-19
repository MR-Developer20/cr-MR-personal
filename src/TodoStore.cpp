#include "TodoStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_random.h>

#include <utility>

TodoStore::TodoStore() : todoList(&esp_random) {}

void TodoStore::toJson(JsonDocument& doc) const {
  doc["version"] = FORMAT_VERSION;
  doc["rev"] = todoList.revision();

  char idHex[17];
  JsonArray live = doc["items"].to<JsonArray>();
  for (const auto& item : todoList.liveItems()) {
    JsonObject obj = live.add<JsonObject>();
    TodoList::idToHex(item.id, idHex);
    obj["id"] = idHex;  // char[] (non-const) is copied by ArduinoJson, so reusing the buffer is safe
    // const char* is stored by reference, not copied: the list does not change while this document is
    // serialized (the caller holds the render lock), and it saves up to 12 KB of heap at 100 items.
    obj["text"] = item.text.c_str();
    obj["done"] = item.done;
    obj["rev"] = item.rev;
    obj["seq"] = item.seq;
    obj["order"] = item.order;
  }

  JsonArray gone = doc["deleted"].to<JsonArray>();
  for (const auto& item : todoList.tombstones()) {
    JsonObject obj = gone.add<JsonObject>();
    TodoList::idToHex(item.id, idHex);
    obj["id"] = idHex;
    obj["rev"] = item.rev;
    obj["seq"] = item.seq;
  }
}

bool TodoStore::fromJson(JsonVariantConst doc) {
  const int version = doc["version"].as<int>();
  if (version > FORMAT_VERSION) {
    LOG_ERR("TODO", "todo.json format %d is newer than supported (%d)", version, FORMAT_VERSION);
    return false;
  }

  // Missing numbers/bools read as 0/false; a missing array is empty (null JsonArray iterates zero times).
  const JsonArrayConst live = doc["items"].as<JsonArrayConst>();
  const JsonArrayConst gone = doc["deleted"].as<JsonArrayConst>();
  todoList.beginLoad(doc["rev"].as<uint32_t>(), live.size(), gone.size());

  int rejected = 0;
  for (JsonObjectConst obj : live) {
    TodoItem item;
    if (!TodoList::hexToId(obj["id"] | "", item.id)) {
      ++rejected;
      continue;
    }
    item.text = obj["text"] | "";
    item.done = obj["done"].as<bool>();
    item.rev = obj["rev"].as<uint32_t>();
    item.seq = obj["seq"].as<uint32_t>();
    item.order = obj["order"].as<int32_t>();
    if (!todoList.loadItem(std::move(item))) ++rejected;
  }
  for (JsonObjectConst obj : gone) {
    TodoItem item;
    if (!TodoList::hexToId(obj["id"] | "", item.id)) {
      ++rejected;
      continue;
    }
    item.rev = obj["rev"].as<uint32_t>();
    item.seq = obj["seq"].as<uint32_t>();
    if (!todoList.loadTombstone(std::move(item))) ++rejected;
  }
  todoList.endLoad();

  if (rejected > 0) {
    LOG_ERR("TODO", "Skipped %d invalid entries in todo.json", rejected);
  }
  LOG_DBG("TODO", "Loaded %d items, %d tombstones (rev %lu)", static_cast<int>(todoList.size()),
          static_cast<int>(todoList.tombstones().size()), static_cast<unsigned long>(todoList.revision()));
  return true;
}

bool TodoStore::parseIncoming(JsonVariantConst body, TodoIncoming& out, const char*& error) {
  // Every failure empties `out`, so a rejected request never keeps its reserved memory.
  const auto fail = [&out, &error](const char* reason) {
    out = TodoIncoming{};
    error = reason;
    return false;
  };
  error = "";
  out = TodoIncoming{};
  if (!body.is<JsonObjectConst>()) return fail("body must be a JSON object");
  // A missing version reads as 0 and is rejected like any other unsupported one.
  if (body["version"].as<int>() != FORMAT_VERSION) return fail("unsupported version");

  // baseRev decides whether the phone is current, so a value that does not fit a u32 must not quietly read
  // as 0 (which would pass on a fresh list). A missing baseRev reads as 0, as C1 says for numbers.
  const JsonVariantConst baseRev = body["baseRev"];
  if (!baseRev.isNull() && !baseRev.is<uint32_t>()) return fail("bad baseRev");

  const JsonArrayConst live = body["items"].as<JsonArrayConst>();
  const JsonArrayConst gone = body["deleted"].as<JsonArrayConst>();
  // Bound the allocation before making it: the limits are the contract's, not the parser's.
  if (live.size() > TodoList::MAX_ITEMS) return fail("too many items");
  if (gone.size() > TodoList::MAX_TOMBSTONES) return fail("too many tombstones");
  out.baseRev = baseRev.as<uint32_t>();
  out.items.reserve(live.size());
  out.deleted.reserve(gone.size());

  for (JsonVariantConst entry : live) {
    const JsonObjectConst obj = entry.as<JsonObjectConst>();
    TodoItem item;
    if (obj.isNull() || !TodoList::hexToId(obj["id"] | "", item.id)) return fail("bad id");
    // Sanitize at the point of copy: the raw text (up to the 32 KB body) is never stored.
    item.text = TodoList::sanitizeText(obj["text"] | "");
    item.done = obj["done"].as<bool>();
    item.rev = obj["rev"].as<uint32_t>();
    item.order = obj["order"].as<int32_t>();  // seq is device-local and deliberately ignored
    out.items.push_back(std::move(item));
  }
  for (JsonVariantConst entry : gone) {
    const JsonObjectConst obj = entry.as<JsonObjectConst>();
    TodoItem item;
    if (obj.isNull() || !TodoList::hexToId(obj["id"] | "", item.id)) return fail("bad id");
    item.rev = obj["rev"].as<uint32_t>();
    out.deleted.push_back(std::move(item));
  }
  return true;
}

void TodoStore::load() {
  dirty = false;
  loaded = true;
  if (loadFromFile()) {
    todoList.reserveCapacity();
    return;
  }

  todoList.clear();
  if (Storage.exists(getFilePath())) {
    // The file exists but could not be used. Keep it so the next save can't overwrite
    // the user's list with an empty one.
    Storage.remove(getBackupPath());
    if (Storage.rename(getFilePath(), getBackupPath())) {
      LOG_ERR("TODO", "Unreadable todo.json moved to todo.json.bak");
    } else {
      LOG_ERR("TODO", "Unreadable todo.json could not be backed up");
    }
  }
  todoList.reserveCapacity();
}

bool TodoStore::saveIfDirty() {
  if (!loaded || !dirty) return true;
  LOG_DBG("TODO", "saving: free heap %u, max block %u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (!saveToFile()) {
    LOG_ERR("TODO", "Failed to save to-do list");
    return false;
  }
  dirty = false;
  return true;
}

void TodoStore::unload() {
  todoList.clear();
  loaded = false;
  dirty = false;
}
