#include "TodoSync.h"

#include <Memory.h>

#include <cstring>

namespace todosync {

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void putU32(uint8_t* out, const uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | static_cast<uint32_t>(in[1]) << 8 | static_cast<uint32_t>(in[2]) << 16 |
         static_cast<uint32_t>(in[3]) << 24;
}

bool parseFrame(const uint8_t* data, const size_t len, FrameView& out) {
  if (len < FRAME_HEADER_BYTES) return false;
  out.type = data[0];
  out.seq = static_cast<uint16_t>(data[1] | data[2] << 8);
  out.payload = data + FRAME_HEADER_BYTES;
  out.payloadLen = len - FRAME_HEADER_BYTES;
  return true;
}

size_t writeHeader(uint8_t* out, const FrameType type, const uint16_t seq) {
  out[0] = static_cast<uint8_t>(type);
  out[1] = static_cast<uint8_t>(seq);
  out[2] = static_cast<uint8_t>(seq >> 8);
  return FRAME_HEADER_BYTES;
}

Status PutAssembler::fail(const Status status) {
  reset();
  return status;
}

void PutAssembler::reset() {
  state = State::Idle;
  buffer.reset();
  total = 0;
  written = 0;
  expectedCrc = 0;
  nextSeq = 0;
}

std::unique_ptr<uint8_t[]> PutAssembler::release() {
  std::unique_ptr<uint8_t[]> out = std::move(buffer);
  reset();
  return out;
}

Status PutAssembler::begin(const uint32_t totalLen, const uint32_t crc) {
  reset();
  if (totalLen == 0) return Status::Invalid;
  if (totalLen > MAX_BODY_BYTES) return Status::TooLarge;
  buffer = makeUniqueNoThrow<uint8_t[]>(totalLen);
  if (!buffer) return Status::Busy;
  total = totalLen;
  expectedCrc = crc;
  state = State::Receiving;
  return Status::Ok;
}

Status PutAssembler::chunk(const uint16_t seq, const uint8_t* data, const size_t len) {
  if (state != State::Receiving) return fail(Status::Invalid);
  if (seq != nextSeq) return fail(Status::Sequence);
  if (len > total - written) return fail(Status::Invalid);
  if (len > 0) memcpy(buffer.get() + written, data, len);
  written += len;
  nextSeq++;
  return Status::Ok;
}

Status PutAssembler::commit() {
  if (state != State::Receiving) return fail(Status::Invalid);
  if (written != total) return fail(Status::Invalid);
  if (crc32(0, buffer.get(), total) != expectedCrc) return fail(Status::CrcMismatch);
  state = State::Complete;
  return Status::Ok;
}

FrameWriter::FrameWriter(uint8_t* frameBuffer, const size_t frameCapacity, const SendFn send, void* ctx)
    : frame(frameBuffer), capacity(frameCapacity), send(send), ctx(ctx) {}

bool FrameWriter::flush() {
  if (broken) return false;
  if (fill == FRAME_HEADER_BYTES) return true;
  writeHeader(frame, FrameType::Chunk, seq);
  if (!send(ctx, frame, fill)) {
    broken = true;
    return false;
  }
  seq++;
  fill = FRAME_HEADER_BYTES;
  return true;
}

bool FrameWriter::write(const uint8_t* data, size_t len) {
  if (broken || capacity <= FRAME_HEADER_BYTES) return false;
  while (len > 0) {
    const size_t room = capacity - fill;
    const size_t take = len < room ? len : room;
    memcpy(frame + fill, data, take);
    fill += take;
    total += take;
    crc = crc32(crc, data, take);
    data += take;
    len -= take;
    if (fill == capacity && !flush()) return false;
  }
  return true;
}

bool FrameWriter::finish() {
  if (!flush()) return false;
  if (capacity < FRAME_HEADER_BYTES + 8) {
    broken = true;
    return false;
  }
  writeHeader(frame, FrameType::End, seq);
  putU32(frame + FRAME_HEADER_BYTES, static_cast<uint32_t>(total));
  putU32(frame + FRAME_HEADER_BYTES + 4, crc);
  if (!send(ctx, frame, FRAME_HEADER_BYTES + 8)) {
    broken = true;
    return false;
  }
  return true;
}

}  // namespace todosync
