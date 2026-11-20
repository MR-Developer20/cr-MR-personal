#include "TodoSyncService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BlePageTurner.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "TodoStore.h"
#include "activities/RenderLock.h"

namespace {

constexpr char TAG[] = "TSYNC";
constexpr char SERVICE_UUID[] = "8d4d5e22-336a-4fa2-bbdf-d7d1d6c90cf1";
constexpr char INFO_UUID[] = "648fb5e2-f2fd-43e7-8a21-f7a6be62e27c";
constexpr char COMMAND_UUID[] = "11c415fe-5334-40c6-b168-48ee5b1a4036";
constexpr char DATA_UUID[] = "1a2cebb4-cbf0-45fc-9d53-f0564b7e87bc";
constexpr char DEVICE_NAME[] = "X4 To-do";

constexpr uint16_t WANTED_MTU = 247;
constexpr uint16_t NO_CONNECTION = BLE_HS_CONN_HANDLE_NONE;
// Heap the stack takes once it runs: 73,656 B free before and 22,676 B after on an X3 (lib/BlePageTurner
// RadioPolicy.h), rounded up. The largest block the stack needs is RadioPolicy kMinimumLargestBlockBytes.
constexpr size_t STACK_BYTES = 51200;
constexpr size_t STACK_LARGEST_BLOCK = 32768;
// What the document serialization needs on top of the list that is already loaded.
constexpr size_t SERIALIZE_BYTES = 24576;
// Heap that stays free while a request is parsed, so the Bluetooth host task can still allocate.
constexpr size_t JSON_FLOOR_BYTES = 12288;
constexpr int NOTIFY_RETRIES = 400;  // About 2 s of 5 ms waits when the controller has no buffer free.

constexpr size_t ERROR_MESSAGE_MAX = 40;

// Writes JSON straight into CHUNK frames.
class FramePrint final : public Print {
 public:
  explicit FramePrint(todosync::FrameWriter& writer) : writer(writer) {}
  size_t write(const uint8_t byte) override { return writer.write(&byte, 1) ? 1 : 0; }
  size_t write(const uint8_t* buffer, const size_t size) override { return writer.write(buffer, size) ? size : 0; }

 private:
  todosync::FrameWriter& writer;
};

class MutexLock {
 public:
  explicit MutexLock(void* handle) : handle(static_cast<SemaphoreHandle_t>(handle)) {
    if (this->handle) xSemaphoreTake(this->handle, portMAX_DELAY);
  }
  ~MutexLock() {
    if (handle) xSemaphoreGive(handle);
  }
  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

 private:
  SemaphoreHandle_t handle;
};

// The body, a JsonDocument built from it and the list being replaced coexist, so a body is accepted only when
// about three times its length fits in the heap above JSON_FLOOR_BYTES. This is also the maxBody that the
// Info characteristic reports.
size_t acceptableBodyBytes() {
  const size_t freeBytes = ESP.getFreeHeap();
  size_t limit = freeBytes > JSON_FLOOR_BYTES ? (freeBytes - JSON_FLOOR_BYTES) / 3 : 0;
  if (limit > todosync::MAX_BODY_BYTES) limit = todosync::MAX_BODY_BYTES;
  const size_t block = ESP.getMaxAllocHeap();
  return block < limit ? block : limit;
}

// Lets a JsonDocument grow only while JSON_FLOOR_BYTES of heap stay free, so a buggy or hostile body cannot
// starve the Bluetooth host task. ArduinoJson owns each block and gives it back through deallocate().
class HeapFloorAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(const size_t size) override { return fits(size) ? malloc(size) : nullptr; }
  void deallocate(void* ptr) override { free(ptr); }
  void* reallocate(void* ptr, const size_t size) override { return fits(size) ? realloc(ptr, size) : nullptr; }

 private:
  static bool fits(const size_t size) { return ESP.getFreeHeap() >= size + JSON_FLOOR_BYTES; }
};

// Read by the host task, cleared by the loop task before the stack goes down.
std::atomic<TodoSyncService*> activeService{nullptr};

}  // namespace

// Reaches the service's private state from the NimBLE callbacks.
struct TodoSyncCallbacks {
  static void connected(TodoSyncService& s, const uint16_t handle, const uint16_t mtu) {
    s.connHandle.store(handle);
    s.mtu.store(mtu);
    s.currentState.store(TodoSyncService::State::Connected);
    s.infoStale.store(true);
    s.changed.store(true);
  }

  static void disconnected(TodoSyncService& s) {
    s.connHandle.store(NO_CONNECTION);
    // A command queued but not started belongs to the lost link; one poll() already took keeps busy set.
    if (s.pending.exchange(TodoSyncService::Pending::None) != TodoSyncService::Pending::None) s.busy.store(false);
    {
      MutexLock lock(s.putMutex);
      s.put.reset();
    }
    if (s.currentState.load() != TodoSyncService::State::Stopped) {
      s.currentState.store(TodoSyncService::State::Advertising);
    }
    s.changed.store(true);
  }

  static void mtuChanged(TodoSyncService& s, const uint16_t mtu) { s.mtu.store(mtu); }
  static void command(TodoSyncService& s, const uint8_t* bytes, const size_t len) { s.onCommand(bytes, len); }
};

namespace {

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    if (TodoSyncService* service = activeService.load()) {
      TodoSyncCallbacks::connected(*service, connInfo.getConnHandle(), connInfo.getMTU());
    }
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    if (TodoSyncService* service = activeService.load()) TodoSyncCallbacks::disconnected(*service);
  }
  void onMTUChange(const uint16_t mtu, NimBLEConnInfo&) override {
    if (TodoSyncService* service = activeService.load()) TodoSyncCallbacks::mtuChanged(*service, mtu);
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    TodoSyncService* service = activeService.load();
    if (!service) return;
    const NimBLEAttValue value = characteristic->getValue();
    TodoSyncCallbacks::command(*service, value.data(), value.size());
  }
};

ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TodoSyncService::setResult(const Result value) {
  lastResult.store(value);
  changed.store(true);
}

bool TodoSyncService::start() {
  if (state() != State::Stopped) return true;
  setResult(Result::None);

  const bleturner::Status turner = bleturner::status();
  if (!turner.compiledIn || turner.running || turner.stopping || turner.starting || NimBLEDevice::isInitialized()) {
    LOG_ERR(TAG, "Bluetooth stack is in use by the page turner");
    setResult(Result::RadioBusy);
    return false;
  }
  if (ESP.getFreeHeap() < STACK_BYTES + SERIALIZE_BYTES || ESP.getMaxAllocHeap() < STACK_LARGEST_BLOCK) {
    LOG_ERR(TAG, "Not enough heap for the stack: free %u largest %u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    setResult(Result::LowMemory);
    return false;
  }

  if (!NimBLEDevice::init(DEVICE_NAME)) {
    LOG_ERR(TAG, "NimBLEDevice::init failed");
    setResult(Result::LowMemory);
    return false;
  }
  // From here the stack is ours, so every failure below goes through stop().
  ownsStack = true;
  putMutex = xSemaphoreCreateMutex();
  if (!putMutex) {
    LOG_ERR(TAG, "Mutex allocation failed");
    stop();
    setResult(Result::LowMemory);
    return false;
  }
  NimBLEDevice::setMTU(WANTED_MTU);

  // A fresh code per visit; 123456 is NimBLE's "ask the callback" default, so skip it.
  do {
    pairingCode = esp_random() % 1000000;
  } while (pairingCode == 123456);
  NimBLEDevice::setSecurityPasskey(pairingCode);
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  activeService.store(this);
  connHandle.store(NO_CONNECTION);
  pending.store(Pending::None);
  busy.store(false);
  refusedBytes.store(0);

  server = NimBLEDevice::createServer();
  NimBLEService* service = server ? server->createService(SERVICE_UUID) : nullptr;
  if (service) {
    info = service->createCharacteristic(INFO_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
    NimBLECharacteristic* command = service->createCharacteristic(
        COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
    data = service->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
    if (command) command->setCallbacks(&commandCallbacks);
    if (!info || !command || !data) service = nullptr;
  }
  if (!service) {
    LOG_ERR(TAG, "GATT service creation failed");
    stop();
    setResult(Result::LowMemory);
    return false;
  }
  server->setCallbacks(&serverCallbacks, /*deleteCallbacks=*/false);
  refreshInfo();
  if (!server->start()) {
    LOG_ERR(TAG, "GATT server start failed");
    stop();
    setResult(Result::LowMemory);
    return false;
  }

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advertisement.addServiceUUID(SERVICE_UUID);
  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(DEVICE_NAME);
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(scanResponse);
  advertising->enableScanResponse(true);
  advertiseUntilMs = millis() + ADVERTISE_MS;
  if (!advertising->start(ADVERTISE_MS)) {
    LOG_ERR(TAG, "Advertising start failed");
    stop();
    setResult(Result::RadioBusy);
    return false;
  }

  LOG_DBG(TAG, "Advertising; free heap %u largest %u with the stack up", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  // The reply document needs this much once the stack and the GATT server have taken their share.
  if (ESP.getFreeHeap() < SERIALIZE_BYTES) {
    LOG_ERR(TAG, "Not enough heap left after the stack: free %u", static_cast<unsigned>(ESP.getFreeHeap()));
    stop();
    setResult(Result::LowMemory);
    return false;
  }
  currentState.store(State::Advertising);
  changed.store(true);
  return true;
}

void TodoSyncService::stop() {
  // Never tear down a stack another owner started (start() refused it).
  if (!ownsStack) {
    currentState.store(State::Stopped);
    return;
  }

  NimBLEDevice::getAdvertising()->stop();
  if (server && server->getConnectedCount() > 0) {
    for (const uint16_t handle : server->getPeerDevices()) server->disconnect(handle);
    for (int i = 0; i < 50 && server->getConnectedCount() > 0; i++) delay(10);
  }
  // Callbacks must not reach this object once the stack is going down.
  activeService.store(nullptr);
  currentState.store(State::Stopped);
  {
    MutexLock lock(putMutex);
    put.reset();
  }
  NimBLEDevice::deinit(/*clearAll=*/true);
  if (NimBLEDevice::isInitialized()) LOG_ERR(TAG, "NimBLE deinit did not complete");
  // The host task is gone, so nothing can wait on the mutex any more.
  if (putMutex) {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(putMutex));
    putMutex = nullptr;
  }
  ownsStack = false;
  server = nullptr;
  info = nullptr;
  data = nullptr;
  LOG_DBG(TAG, "Stopped; free heap %u largest %u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

bool TodoSyncService::poll() {
  if (state() == State::Stopped) return false;

  if (infoStale.exchange(false)) refreshInfo();
  if (const uint32_t refused = refusedBytes.exchange(0)) {
    LOG_ERR(TAG, "Refused a %u byte body: free %u largest %u, acceptable %u", static_cast<unsigned>(refused),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(acceptableBodyBytes()));
    setResult(Result::LowMemory);
  }

  // busy was set by the host task when it queued the command; it stays set until the work is done.
  const Pending command = pending.exchange(Pending::None);
  if (command != Pending::None) {
    currentState.store(State::Working);
    changed.store(true);
    if (command == Pending::Get) {
      runGet();
    } else {
      runCommit();
    }
    // The link may have dropped while we worked.
    currentState.store(connHandle.load() == NO_CONNECTION ? State::Advertising : State::Connected);
    busy.store(false);
  }

  const int32_t remaining = static_cast<int32_t>(advertiseUntilMs - millis());
  if (state() == State::Advertising) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (remaining <= 0) {
      // Advertising ran out: free the stack so the device can sleep, and keep the message on screen.
      if (result() == Result::None) lastResult.store(Result::TimedOut);
      stop();
      changed.store(true);
    } else if (!advertising->isAdvertising()) {
      advertising->start(static_cast<uint32_t>(remaining));
    }
  }
  return changed.exchange(false);
}

// ---------------------------------------------------------------------------
// Frames out
// ---------------------------------------------------------------------------

size_t TodoSyncService::frameCapacity() const {
  const size_t byLink = mtu.load() > 3 ? static_cast<size_t>(mtu.load()) - 3 : 0;
  return byLink < sizeof(frameBuffer) ? byLink : sizeof(frameBuffer);
}

// The loop task may wait for a controller buffer; the host task makes one attempt, because it is the task
// that frees those buffers.
bool TodoSyncService::notifyFrame(const uint8_t* frame, const size_t len, const int attempts) {
  for (int attempt = 0; attempt < attempts; attempt++) {
    const uint16_t handle = connHandle.load();
    if (handle == NO_CONNECTION || !data) return false;
    if (data->notify(frame, len, handle)) return true;
    if (attempts > 1) delay(5);  // No controller buffer free yet.
  }
  if (attempts > 1) LOG_ERR(TAG, "Notify gave up after %d tries", attempts);
  return false;
}

bool TodoSyncService::sendThunk(void* ctx, const uint8_t* frame, const size_t len) {
  return static_cast<TodoSyncService*>(ctx)->notifyFrame(frame, len, NOTIFY_RETRIES);
}

void TodoSyncService::sendError(const todosync::Status code, const char* message, const bool fromHostTask) {
  uint8_t frame[todosync::FRAME_HEADER_BYTES + 1 + ERROR_MESSAGE_MAX];
  size_t len = todosync::writeHeader(frame, todosync::FrameType::Err, 0);
  frame[len++] = static_cast<uint8_t>(code);
  // The frame must fit in ATT_MTU - 3.
  const size_t limit = frameCapacity() < sizeof(frame) ? frameCapacity() : sizeof(frame);
  const size_t room = limit > len ? limit - len : 0;
  const size_t textLen = message ? strnlen(message, room) : 0;
  if (textLen > 0) memcpy(frame + len, message, textLen);
  len += textLen;
  notifyFrame(frame, len, fromHostTask ? 1 : NOTIFY_RETRIES);
}

void TodoSyncService::sendOk(const uint32_t revision) {
  uint8_t frame[todosync::FRAME_HEADER_BYTES + 4];
  todosync::writeHeader(frame, todosync::FrameType::Ok, 0);
  todosync::putU32(frame + todosync::FRAME_HEADER_BYTES, revision);
  notifyFrame(frame, sizeof(frame), NOTIFY_RETRIES);
}

void TodoSyncService::refreshInfo() {
  if (!info) return;
  char json[256];
  const int n = snprintf(json, sizeof(json),
                         "{\"proto\":1,\"fw\":\"%s\",\"rev\":%u,\"maxItems\":%u,\"maxText\":%u,\"maxBody\":%u}",
                         CROSSPOINT_VERSION, static_cast<unsigned>(TODO_STORE.list().revision()),
                         static_cast<unsigned>(TodoList::MAX_ITEMS), static_cast<unsigned>(TodoList::MAX_TEXT_BYTES),
                         static_cast<unsigned>(acceptableBodyBytes()));
  if (n > 0 && static_cast<size_t>(n) < sizeof(json)) info->setValue(reinterpret_cast<const uint8_t*>(json), n);
}

// The list is not edited while this screen is open (TodoActivity is paused beneath it), so item text that
// TodoStore::toJson keeps by reference stays valid until serialization ends.
bool TodoSyncService::streamDocument() {
  if (ESP.getFreeHeap() < SERIALIZE_BYTES) {
    LOG_ERR(TAG, "Not enough heap to serialize: free %u", static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  JsonDocument doc;
  TODO_STORE.toJson(doc);
  if (doc.overflowed()) {
    LOG_ERR(TAG, "Document does not fit in memory");
    return false;
  }
  todosync::FrameWriter writer(frameBuffer, frameCapacity(), &TodoSyncService::sendThunk, this);
  FramePrint out(writer);
  serializeJson(doc, out);
  return writer.finish();
}

// ---------------------------------------------------------------------------
// Commands in (host task)
// ---------------------------------------------------------------------------

void TodoSyncService::onCommand(const uint8_t* bytes, const size_t len) {
  todosync::FrameView frame;
  if (!todosync::parseFrame(bytes, len, frame)) {
    sendError(todosync::Status::Invalid, "short frame", true);
    return;
  }

  switch (static_cast<todosync::FrameType>(frame.type)) {
    case todosync::FrameType::Get:
      if (busy.exchange(true)) {
        sendError(todosync::Status::Busy, "busy", true);
        return;
      }
      {
        MutexLock lock(putMutex);
        put.reset();  // A new GET aborts any partial PUT.
      }
      pending.store(Pending::Get);
      break;
    case todosync::FrameType::PutBegin:
      onPutBegin(frame);
      break;
    case todosync::FrameType::PutChunk: {
      todosync::Status status;
      {
        MutexLock lock(putMutex);
        status = put.chunk(frame.seq, frame.payload, frame.payloadLen);
      }
      if (status != todosync::Status::Ok) sendError(status, "bad chunk", true);
      break;
    }
    case todosync::FrameType::PutCommit: {
      bool receiving;
      {
        MutexLock lock(putMutex);
        receiving = put.receiving();
      }
      if (!receiving) {
        sendError(todosync::Status::Invalid, "no transfer", true);
        return;
      }
      if (busy.exchange(true)) {
        sendError(todosync::Status::Busy, "busy", true);
        return;
      }
      pending.store(Pending::Commit);
      break;
    }
    default:
      sendError(todosync::Status::Invalid, "unknown type", true);
      break;
  }
}

void TodoSyncService::onPutBegin(const todosync::FrameView& frame) {
  if (frame.payloadLen != 8) {
    sendError(todosync::Status::Invalid, "bad begin", true);
    return;
  }
  if (busy.load()) {
    sendError(todosync::Status::Busy, "busy", true);
    return;
  }
  const uint32_t totalLen = todosync::getU32(frame.payload);
  const uint32_t crc = todosync::getU32(frame.payload + 4);
  todosync::Status status = todosync::Status::Ok;
  if (totalLen > todosync::MAX_BODY_BYTES) {
    status = todosync::Status::TooLarge;
  } else if (totalLen > acceptableBodyBytes()) {
    refusedBytes.store(totalLen);  // Logged by poll(): the host task has a small stack.
    status = todosync::Status::Busy;
  } else {
    MutexLock lock(putMutex);
    status = put.begin(totalLen, crc);
  }
  if (status != todosync::Status::Ok) {
    {
      MutexLock lock(putMutex);
      put.reset();
    }
    sendError(status, status == todosync::Status::Busy ? "out of memory" : "bad begin", true);
  }
}

// ---------------------------------------------------------------------------
// Work (loop task)
// ---------------------------------------------------------------------------

void TodoSyncService::runGet() {
  if (connHandle.load() == NO_CONNECTION) return;
  LOG_DBG(TAG, "GET: free heap %u largest %u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  // A dropped link is not a memory problem: leave the result as it was.
  if (!streamDocument() && connHandle.load() != NO_CONNECTION) {
    sendError(todosync::Status::Busy, "out of memory");
    setResult(Result::LowMemory);
  }
}

// Parses and applies a received body. Large buffers live in separate scopes so they are gone before the
// reply is serialized: the request body, then the JsonDocument built from it, then the parsed list.
todosync::Status TodoSyncService::applyBody(std::unique_ptr<uint8_t[]> body, const size_t len, const char*& message) {
  message = "";
  TodoIncoming incoming;
  {
    HeapFloorAllocator allocator;
    JsonDocument doc(&allocator);
    const DeserializationError parsed = deserializeJson(doc, reinterpret_cast<const char*>(body.get()), len);
    body.reset();
    if (parsed == DeserializationError::NoMemory) {
      message = "out of memory";
      return todosync::Status::Busy;
    }
    if (parsed) {
      message = "bad json";
      return todosync::Status::Invalid;
    }
    if (!TodoStore::parseIncoming(doc.as<JsonVariantConst>(), incoming, message)) return todosync::Status::Invalid;
  }

  // The loop task holds the render lock for the apply and the write: the SD card shares SPI with the display.
  RenderLock lock;
  const TodoApplyResult applied = TODO_STORE.list().applyIncoming(std::move(incoming));
  switch (applied.status) {
    case TodoApplyStatus::Applied:
      TODO_STORE.markDirty();
      if (!TODO_STORE.saveIfDirty()) {
        message = "save failed";
        return todosync::Status::Busy;
      }
      return todosync::Status::Ok;
    case TodoApplyStatus::Unchanged:
      return todosync::Status::Ok;
    case TodoApplyStatus::Conflict:
      return todosync::Status::Conflict;
    case TodoApplyStatus::Invalid:
      message = applied.reason;
      return todosync::Status::Invalid;
  }
  return todosync::Status::Invalid;
}

void TodoSyncService::runCommit() {
  std::unique_ptr<uint8_t[]> body;
  size_t len = 0;
  todosync::Status status;
  {
    MutexLock lock(putMutex);
    status = put.commit();
    if (status == todosync::Status::Ok) {
      len = put.size();
      body = put.release();
    }
  }
  if (status != todosync::Status::Ok) {
    // The link dropped and cleared the transfer: nothing to answer.
    if (connHandle.load() == NO_CONNECTION) return;
    sendError(status, status == todosync::Status::CrcMismatch ? "crc mismatch" : "bad transfer");
    setResult(Result::Rejected);
    return;
  }

  LOG_DBG(TAG, "PUT %u bytes: free heap %u largest %u", static_cast<unsigned>(len),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  const char* message = "";
  status = applyBody(std::move(body), len, message);

  switch (status) {
    case todosync::Status::Ok:
      // The list is saved, so the sync worked even if the phone drops before the reply is complete.
      refreshInfo();
      setResult(Result::Synced);
      sendOk(TODO_STORE.list().revision());
      if (!streamDocument() && connHandle.load() != NO_CONNECTION) setResult(Result::LowMemory);
      break;
    case todosync::Status::Conflict:
      sendError(status, "conflict");
      streamDocument();
      setResult(Result::Conflict);
      break;
    case todosync::Status::Busy:
      sendError(status, message);
      setResult(strcmp(message, "save failed") == 0 ? Result::SaveFailed : Result::LowMemory);
      break;
    default:
      sendError(status, message);
      setResult(Result::Rejected);
      break;
  }
}
