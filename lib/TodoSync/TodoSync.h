#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

/**
 * Transport-independent half of the to-do Bluetooth sync (docs/todo-sync-protocol.md, C7): frames, CRC-32,
 * the receiver for a phone's PUT, and the writer for the device's reply. No Arduino, BLE or SD
 * dependencies, so it is unit-tested on the host (test/todo_sync).
 */
namespace todosync {

constexpr size_t FRAME_HEADER_BYTES = 3;  // [type u8][seq u16 LE]
constexpr size_t MAX_BODY_BYTES = 32768;  // C2: request body limit.

enum class FrameType : uint8_t {
  Get = 0x01,
  Chunk = 0x02,
  End = 0x03,
  PutBegin = 0x10,
  PutChunk = 0x11,
  PutCommit = 0x12,
  Ok = 0x20,
  Err = 0x7F,
};

// ERR codes on the wire (Ok is never sent as an ERR).
enum class Status : uint8_t {
  Ok = 0,
  Conflict = 1,
  Invalid = 2,
  TooLarge = 3,
  CrcMismatch = 4,
  Sequence = 5,
  Busy = 6,  // Busy or out of memory.
};

// IEEE / zlib CRC-32. Start with crc = 0 and feed the result back in to continue a running value.
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);

void putU32(uint8_t* out, uint32_t value);  // Little-endian.
uint32_t getU32(const uint8_t* in);

struct FrameView {
  uint8_t type = 0;
  uint16_t seq = 0;
  const uint8_t* payload = nullptr;  // Points into the caller's buffer.
  size_t payloadLen = 0;
};

// False when `len` is shorter than the header.
bool parseFrame(const uint8_t* data, size_t len, FrameView& out);
// Writes the 3-byte header and returns its size.
size_t writeHeader(uint8_t* out, FrameType type, uint16_t seq);

/**
 * Receives one PUT: begin(), chunk() x n, commit(). The buffer is sized from the announced length, so the
 * caller checks the heap before begin() and answers Busy when it does not fit. Any failure leaves the
 * assembler empty (buffer freed); begin() always starts over.
 */
class PutAssembler {
 public:
  // Allocates `totalLen` bytes. Invalid for 0, TooLarge above MAX_BODY_BYTES, Busy when allocation fails.
  Status begin(uint32_t totalLen, uint32_t expectedCrc);
  // `seq` counts PUT_CHUNK frames from 0. Sequence on a gap or repeat, Invalid on overflow or no begin().
  Status chunk(uint16_t seq, const uint8_t* data, size_t len);
  // Checks length, then CRC. After Ok, data() and size() hold the body until release()/reset().
  Status commit();

  bool receiving() const { return state == State::Receiving; }
  bool complete() const { return state == State::Complete; }
  const uint8_t* data() const { return buffer.get(); }
  size_t size() const { return total; }
  // Gives up the buffer (the body stays valid for whoever takes it).
  std::unique_ptr<uint8_t[]> release();
  void reset();

 private:
  enum class State : uint8_t { Idle, Receiving, Complete };
  State state = State::Idle;
  std::unique_ptr<uint8_t[]> buffer;
  size_t total = 0;
  size_t written = 0;
  uint32_t expectedCrc = 0;
  uint16_t nextSeq = 0;

  Status fail(Status status);
};

/**
 * Turns a byte stream into CHUNK frames and a closing END frame. The caller owns the frame buffer
 * (header + payload, at most ATT_MTU - 3 bytes). `send` returns false when the link refused the frame;
 * after that every call returns false and nothing more is sent.
 */
class FrameWriter {
 public:
  using SendFn = bool (*)(void* ctx, const uint8_t* frame, size_t len);

  FrameWriter(uint8_t* frameBuffer, size_t frameCapacity, SendFn send, void* ctx);

  bool write(const uint8_t* data, size_t len);
  // Sends the pending CHUNK, then END(totalLen, crc32). Returns false if any send failed.
  bool finish();

  size_t totalBytes() const { return total; }
  bool failed() const { return broken; }

 private:
  uint8_t* frame;
  size_t capacity;
  SendFn send;
  void* ctx;
  size_t fill = FRAME_HEADER_BYTES;
  size_t total = 0;
  uint32_t crc = 0;
  uint16_t seq = 0;
  bool broken = false;

  bool flush();
};

}  // namespace todosync
