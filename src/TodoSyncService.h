#pragma once

#include <TodoSync.h>

#include <atomic>
#include <cstdint>

class NimBLECharacteristic;
class NimBLEServer;

/**
 * Bluetooth side of the to-do sync (docs/todo-sync-protocol.md, C7): a one-connection GATT server that is
 * advertised only while TodoSyncActivity is open.
 *
 * It owns the NimBLE stack from start() to stop(), so the page turner must not hold it: start() refuses
 * while lib/BlePageTurner runs, and ActivityManager stops the page turner before every screen change.
 *
 * Threads: the NimBLE host task runs the callbacks. It only moves bytes: it feeds the PUT assembler and
 * queues GET/COMMIT. poll(), on the activity's loop task, does the work that needs the SD card, the render
 * lock or a large buffer.
 */
class TodoSyncService {
 public:
  enum class State : uint8_t { Stopped, Advertising, Connected, Working };
  // What the screen says about the last attempt; None until something happened.
  enum class Result : uint8_t { None, Synced, Conflict, Rejected, LowMemory, SaveFailed, RadioBusy, TimedOut };

  static constexpr uint32_t ADVERTISE_MS = 180000;

  // False: the stack did not start; result() says why.
  bool start();
  void stop();
  // Loop task. Runs a queued command; when advertising times out it stops the stack. True when the screen
  // should repaint.
  bool poll();

  State state() const { return currentState.load(std::memory_order_relaxed); }
  Result result() const { return lastResult.load(std::memory_order_relaxed); }
  uint32_t passkey() const { return pairingCode; }

 private:
  friend struct TodoSyncCallbacks;

  enum class Pending : uint8_t { None, Get, Commit };

  std::atomic<State> currentState{State::Stopped};
  std::atomic<Result> lastResult{Result::None};
  std::atomic<Pending> pending{Pending::None};
  std::atomic<bool> busy{false};          // A GET/COMMIT is queued or running; the host task refuses new ones.
  std::atomic<bool> infoStale{false};     // Info should be rewritten: the heap or the revision changed.
  std::atomic<uint32_t> refusedBytes{0};  // A PUT_BEGIN the heap could not take, for poll() to log.
  std::atomic<bool> changed{false};       // The screen should repaint.
  std::atomic<uint16_t> connHandle{0xFFFF};
  std::atomic<uint16_t> mtu{23};
  bool ownsStack = false;  // True from our NimBLEDevice::init until our deinit.
  uint32_t pairingCode = 0;
  uint32_t advertiseUntilMs = 0;

  NimBLEServer* server = nullptr;
  NimBLECharacteristic* info = nullptr;
  NimBLECharacteristic* data = nullptr;

  todosync::PutAssembler put;  // Guarded by putMutex: the host task writes it, poll() commits it.
  void* putMutex = nullptr;    // FreeRTOS semaphore; created in start().
  uint8_t frameBuffer[244] = {};

  void setResult(Result value);
  size_t frameCapacity() const;
  void refreshInfo();
  bool notifyFrame(const uint8_t* frame, size_t len, int attempts);
  static bool sendThunk(void* ctx, const uint8_t* frame, size_t len);
  void sendError(todosync::Status code, const char* message, bool fromHostTask = false);
  void sendOk(uint32_t revision);
  bool streamDocument();

  // Host task side.
  void onCommand(const uint8_t* bytes, size_t len);
  void onPutBegin(const todosync::FrameView& frame);
  // Loop task side.
  void runGet();
  void runCommit();
  todosync::Status applyBody(std::unique_ptr<uint8_t[]> body, size_t len, const char*& message);
};
