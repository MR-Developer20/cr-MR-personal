#include <TodoSync.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace todosync;

namespace {

const uint8_t* bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

struct Sink {
  std::vector<std::vector<uint8_t>> frames;
  int failAfter = -1;  // Refuse every frame from this index on.
};

bool collect(void* ctx, const uint8_t* frame, size_t len) {
  Sink& sink = *static_cast<Sink*>(ctx);
  if (sink.failAfter >= 0 && static_cast<int>(sink.frames.size()) >= sink.failAfter) return false;
  sink.frames.emplace_back(frame, frame + len);
  return true;
}

// Reassembles the frames a FrameWriter produced, checking seq, END length and CRC like the app does.
bool reassemble(const Sink& sink, std::string& out) {
  out.clear();
  uint16_t expected = 0;
  for (size_t i = 0; i < sink.frames.size(); i++) {
    FrameView f;
    if (!parseFrame(sink.frames[i].data(), sink.frames[i].size(), f)) return false;
    if (f.seq != expected) return false;
    if (f.type == static_cast<uint8_t>(FrameType::Chunk)) {
      out.append(reinterpret_cast<const char*>(f.payload), f.payloadLen);
      expected++;
    } else if (f.type == static_cast<uint8_t>(FrameType::End)) {
      if (i + 1 != sink.frames.size() || f.payloadLen != 8) return false;
      return getU32(f.payload) == out.size() && getU32(f.payload + 4) == crc32(0, bytes(out), out.size());
    } else {
      return false;
    }
  }
  return false;
}

std::string makeBody(size_t n) {
  std::string s(n, 'a');
  for (size_t i = 0; i < n; i++) s[i] = static_cast<char>('a' + (i * 7) % 26);
  return s;
}

}  // namespace

TEST(Crc32, KnownVectors) {
  EXPECT_EQ(crc32(0, nullptr, 0), 0u);
  const std::string v = "123456789";
  EXPECT_EQ(crc32(0, bytes(v), v.size()), 0xCBF43926u);
}

TEST(Crc32, RunningValueMatchesOneShot) {
  const std::string v = makeBody(1000);
  uint32_t running = 0;
  running = crc32(running, bytes(v), 300);
  running = crc32(running, bytes(v) + 300, 700);
  EXPECT_EQ(running, crc32(0, bytes(v), v.size()));
}

TEST(Frames, IntegersAreLittleEndian) {
  uint8_t b[4];
  putU32(b, 0x01020304u);
  EXPECT_EQ(b[0], 0x04);
  EXPECT_EQ(b[3], 0x01);
  EXPECT_EQ(getU32(b), 0x01020304u);
}

TEST(Frames, HeaderRoundTrip) {
  uint8_t buf[8] = {};
  EXPECT_EQ(writeHeader(buf, FrameType::PutChunk, 0x0102), 3u);
  buf[3] = 0xAA;
  FrameView f;
  ASSERT_TRUE(parseFrame(buf, 4, f));
  EXPECT_EQ(f.type, 0x11);
  EXPECT_EQ(f.seq, 0x0102);
  EXPECT_EQ(f.payloadLen, 1u);
  EXPECT_EQ(f.payload[0], 0xAA);
}

TEST(Frames, ShortFrameIsRejected) {
  uint8_t buf[2] = {1, 0};
  FrameView f;
  EXPECT_FALSE(parseFrame(buf, 2, f));
}

TEST(PutAssembler, ReceivesChunksAndCommits) {
  const std::string body = makeBody(1000);
  PutAssembler put;
  ASSERT_EQ(put.begin(1000, crc32(0, bytes(body), body.size())), Status::Ok);
  EXPECT_TRUE(put.receiving());
  uint16_t seq = 0;
  for (size_t off = 0; off < body.size(); off += 200) {
    ASSERT_EQ(put.chunk(seq++, bytes(body) + off, 200), Status::Ok);
  }
  ASSERT_EQ(put.commit(), Status::Ok);
  EXPECT_TRUE(put.complete());
  ASSERT_EQ(put.size(), body.size());
  EXPECT_EQ(memcmp(put.data(), body.data(), body.size()), 0);
}

TEST(PutAssembler, BeginRejectsBadLengths) {
  PutAssembler put;
  EXPECT_EQ(put.begin(0, 0), Status::Invalid);
  EXPECT_EQ(put.begin(MAX_BODY_BYTES + 1, 0), Status::TooLarge);
  EXPECT_EQ(put.begin(MAX_BODY_BYTES, 0), Status::Ok);
}

TEST(PutAssembler, SequenceGapAbortsAndFreesBuffer) {
  PutAssembler put;
  ASSERT_EQ(put.begin(10, 0), Status::Ok);
  const uint8_t b[5] = {};
  ASSERT_EQ(put.chunk(0, b, 5), Status::Ok);
  EXPECT_EQ(put.chunk(2, b, 5), Status::Sequence);
  EXPECT_FALSE(put.receiving());
  EXPECT_EQ(put.data(), nullptr);
}

TEST(PutAssembler, RepeatedSequenceIsRejected) {
  PutAssembler put;
  ASSERT_EQ(put.begin(10, 0), Status::Ok);
  const uint8_t b[5] = {};
  ASSERT_EQ(put.chunk(0, b, 5), Status::Ok);
  EXPECT_EQ(put.chunk(0, b, 5), Status::Sequence);
}

TEST(PutAssembler, OverflowIsInvalid) {
  PutAssembler put;
  ASSERT_EQ(put.begin(8, 0), Status::Ok);
  const uint8_t b[9] = {};
  EXPECT_EQ(put.chunk(0, b, 9), Status::Invalid);
  EXPECT_FALSE(put.receiving());
}

TEST(PutAssembler, ShortCommitIsInvalid) {
  PutAssembler put;
  ASSERT_EQ(put.begin(10, 0), Status::Ok);
  const uint8_t b[4] = {};
  ASSERT_EQ(put.chunk(0, b, 4), Status::Ok);
  EXPECT_EQ(put.commit(), Status::Invalid);
}

TEST(PutAssembler, CrcMismatchIsReportedAndNothingKept) {
  const std::string body = "hello world";
  PutAssembler put;
  ASSERT_EQ(put.begin(static_cast<uint32_t>(body.size()), 0x12345678u), Status::Ok);
  ASSERT_EQ(put.chunk(0, bytes(body), body.size()), Status::Ok);
  EXPECT_EQ(put.commit(), Status::CrcMismatch);
  EXPECT_FALSE(put.complete());
  EXPECT_EQ(put.data(), nullptr);
}

TEST(PutAssembler, ChunkOrCommitWithoutBeginIsInvalid) {
  PutAssembler put;
  const uint8_t b[1] = {};
  EXPECT_EQ(put.chunk(0, b, 1), Status::Invalid);
  EXPECT_EQ(put.commit(), Status::Invalid);
}

TEST(PutAssembler, BeginDiscardsAPartialTransfer) {
  PutAssembler put;
  ASSERT_EQ(put.begin(10, 0), Status::Ok);
  const uint8_t b[4] = {1, 2, 3, 4};
  ASSERT_EQ(put.chunk(0, b, 4), Status::Ok);
  ASSERT_EQ(put.begin(6, 0), Status::Ok);
  EXPECT_EQ(put.size(), 6u);
  EXPECT_EQ(put.chunk(0, b, 4), Status::Ok);  // seq restarted with the new transfer.
}

TEST(PutAssembler, ReleaseHandsOverTheBody) {
  const std::string body = "abc";
  PutAssembler put;
  ASSERT_EQ(put.begin(3, crc32(0, bytes(body), 3)), Status::Ok);
  ASSERT_EQ(put.chunk(0, bytes(body), 3), Status::Ok);
  ASSERT_EQ(put.commit(), Status::Ok);
  std::unique_ptr<uint8_t[]> taken = put.release();
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(memcmp(taken.get(), "abc", 3), 0);
  EXPECT_FALSE(put.complete());
}

TEST(FrameWriter, SplitsAtTheFrameSizeAndClosesWithEnd) {
  const std::string body = makeBody(1000);
  uint8_t frame[23];  // 20 payload bytes per frame (MTU 26).
  Sink sink;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  ASSERT_TRUE(w.write(bytes(body), body.size()));
  ASSERT_TRUE(w.finish());
  EXPECT_EQ(w.totalBytes(), 1000u);
  EXPECT_EQ(sink.frames.size(), 51u);  // 50 chunks + END.
  for (size_t i = 0; i + 1 < sink.frames.size(); i++) EXPECT_LE(sink.frames[i].size(), sizeof(frame));
  std::string out;
  ASSERT_TRUE(reassemble(sink, out));
  EXPECT_EQ(out, body);
}

TEST(FrameWriter, HandlesUnevenWritesAndPartialLastFrame) {
  const std::string body = makeBody(137);
  uint8_t frame[3 + 16];
  Sink sink;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  size_t off = 0;
  const size_t steps[] = {1, 5, 40, 0, 91};
  for (size_t step : steps) {
    ASSERT_TRUE(w.write(bytes(body) + off, step));
    off += step;
  }
  ASSERT_TRUE(w.finish());
  std::string out;
  ASSERT_TRUE(reassemble(sink, out));
  EXPECT_EQ(out, body);
}

TEST(FrameWriter, EmptyDocumentSendsOnlyEnd) {
  uint8_t frame[32];
  Sink sink;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  ASSERT_TRUE(w.finish());
  ASSERT_EQ(sink.frames.size(), 1u);
  FrameView f;
  ASSERT_TRUE(parseFrame(sink.frames[0].data(), sink.frames[0].size(), f));
  EXPECT_EQ(f.type, static_cast<uint8_t>(FrameType::End));
  EXPECT_EQ(getU32(f.payload), 0u);
  EXPECT_EQ(getU32(f.payload + 4), 0u);
}

TEST(FrameWriter, ExactMultipleSendsNoEmptyChunk) {
  const std::string body = makeBody(32);
  uint8_t frame[3 + 16];
  Sink sink;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  ASSERT_TRUE(w.write(bytes(body), body.size()));
  ASSERT_TRUE(w.finish());
  EXPECT_EQ(sink.frames.size(), 3u);  // 2 chunks + END.
  std::string out;
  ASSERT_TRUE(reassemble(sink, out));
  EXPECT_EQ(out, body);
}

TEST(FrameWriter, SendFailureIsSticky) {
  const std::string body = makeBody(100);
  uint8_t frame[3 + 10];
  Sink sink;
  sink.failAfter = 2;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  EXPECT_FALSE(w.write(bytes(body), body.size()));
  EXPECT_TRUE(w.failed());
  EXPECT_FALSE(w.write(bytes(body), 1));
  EXPECT_FALSE(w.finish());
  EXPECT_EQ(sink.frames.size(), 2u);
}

TEST(FrameWriter, TooSmallFrameBufferFails) {
  uint8_t frame[3];
  Sink sink;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  const uint8_t b[1] = {};
  EXPECT_FALSE(w.write(b, 1));
}

TEST(FrameWriter, FrameBufferTooSmallForEndFails) {
  uint8_t frame[3 + 4];
  Sink sink;
  FrameWriter w(frame, sizeof(frame), collect, &sink);
  EXPECT_FALSE(w.finish());
  EXPECT_TRUE(w.failed());
}
