#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

#include "lib/TodoList/TodoList.h"

namespace {

// Deterministic RNG so ids are reproducible.
uint32_t gCounter = 0;
uint32_t countingRng() { return ++gCounter; }

// Always returns the same value, to exercise the duplicate-id fallback.
uint32_t stuckRng() { return 7; }

class TodoListTest : public ::testing::Test {
 protected:
  void SetUp() override { gCounter = 0; }
  TodoList list{&countingRng};
};

TodoItem makeItem(uint64_t id, const std::string& text, int32_t order, uint32_t rev = 1, uint32_t seq = 1) {
  TodoItem item;
  item.id = id;
  item.text = text;
  item.order = order;
  item.rev = rev;
  item.seq = seq;
  return item;
}

// --- sanitizeText ---

TEST(TodoListSanitize, CollapsesWhitespaceAndControlCharacters) {
  EXPECT_EQ(TodoList::sanitizeText("  buy\n\tmilk \r\n and  eggs  "), "buy milk and eggs");
  EXPECT_EQ(TodoList::sanitizeText("a\x7f"
                                   "b"),
            "a b");
}

TEST(TodoListSanitize, BlankInputBecomesEmpty) {
  EXPECT_EQ(TodoList::sanitizeText(""), "");
  EXPECT_EQ(TodoList::sanitizeText(" \n\t "), "");
}

TEST(TodoListSanitize, TruncatesToLimit) {
  const std::string longText(500, 'x');
  EXPECT_EQ(TodoList::sanitizeText(longText).size(), TodoList::MAX_TEXT_BYTES);
}

TEST(TodoListSanitize, NeverSplitsAUtf8Sequence) {
  // 119 ASCII bytes followed by a 2-byte character would straddle the 120-byte limit.
  const std::string text = std::string(119, 'a') + "\xC3\xA9" + "tail";
  const std::string out = TodoList::sanitizeText(text);
  EXPECT_EQ(out, std::string(119, 'a'));
  // 3-byte characters: 40 of them is exactly 120 bytes and must be kept whole.
  std::string euros;
  for (int i = 0; i < 41; ++i) euros += "\xE2\x82\xAC";
  EXPECT_EQ(TodoList::sanitizeText(euros).size(), 120u);
}

TEST(TodoListSanitize, TrimsSpaceLeftAtTheCut) {
  const std::string text = std::string(119, 'a') + " bbbb";
  EXPECT_EQ(TodoList::sanitizeText(text), std::string(119, 'a'));
}

// The contract's C3 table (docs/todo-sync-protocol.md): the phone app must pass the same vectors.
TEST(TodoListSanitize, ContractVectors) {
  EXPECT_EQ(TodoList::sanitizeText("  buy\n\tmilk \r\n and  eggs  "), "buy milk and eggs");
  EXPECT_EQ(TodoList::sanitizeText("a\x7f"
                                   "b"),
            "a b");
  EXPECT_EQ(TodoList::sanitizeText(" \n\t "), "");
  EXPECT_EQ(TodoList::sanitizeText(std::string(500, 'x')), std::string(120, 'x'));
  EXPECT_EQ(TodoList::sanitizeText(std::string(119, 'a') + "\xC3\xA9" + "tail"), std::string(119, 'a'));
  std::string euros41;
  std::string euros40;
  for (int i = 0; i < 41; ++i) euros41 += "\xE2\x82\xAC";
  for (int i = 0; i < 40; ++i) euros40 += "\xE2\x82\xAC";
  EXPECT_EQ(TodoList::sanitizeText(euros41), euros40);
  EXPECT_EQ(euros40.size(), 120u);
  EXPECT_EQ(TodoList::sanitizeText(std::string(119, 'a') + " bbbb"), std::string(119, 'a'));
}

// --- ids ---

TEST(TodoListIds, HexRoundTrip) {
  char hex[17];
  TodoList::idToHex(0x0123456789abcdefULL, hex);
  EXPECT_STREQ(hex, "0123456789abcdef");
  uint64_t id = 0;
  ASSERT_TRUE(TodoList::hexToId(hex, id));
  EXPECT_EQ(id, 0x0123456789abcdefULL);
  ASSERT_TRUE(TodoList::hexToId("0123456789ABCDEF", id));
  EXPECT_EQ(id, 0x0123456789abcdefULL);
}

TEST(TodoListIds, RejectsBadHex) {
  uint64_t id = 42;
  EXPECT_FALSE(TodoList::hexToId("", id));
  EXPECT_FALSE(TodoList::hexToId("123", id));
  EXPECT_FALSE(TodoList::hexToId("0123456789abcdeg", id));
  EXPECT_FALSE(TodoList::hexToId("0000000000000000", id));  // zero is reserved
  EXPECT_EQ(id, 42u);
}

TEST(TodoListIds, StuckRngStillProducesUniqueIds) {
  TodoList stuck{&stuckRng};
  for (int i = 0; i < 5; ++i) ASSERT_EQ(stuck.add("item"), TodoEditResult::Ok);
  for (size_t i = 0; i < stuck.size(); ++i) {
    for (size_t j = i + 1; j < stuck.size(); ++j) EXPECT_NE(stuck.at(i).id, stuck.at(j).id);
  }
}

// --- edits ---

TEST_F(TodoListTest, AddAppendsInOrderAndBumpsRevisions) {
  ASSERT_EQ(list.add("first"), TodoEditResult::Ok);
  ASSERT_EQ(list.add("second"), TodoEditResult::Ok);
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list.at(0).text, "first");
  EXPECT_LT(list.at(0).order, list.at(1).order);
  EXPECT_EQ(list.at(0).rev, 1u);
  EXPECT_EQ(list.revision(), 2u);
  EXPECT_NE(list.at(0).id, 0u);
}

TEST_F(TodoListTest, AddRejectsEmptyAndFull) {
  EXPECT_EQ(list.add("   "), TodoEditResult::EmptyText);
  for (size_t i = 0; i < TodoList::MAX_ITEMS; ++i) ASSERT_EQ(list.add("x"), TodoEditResult::Ok);
  EXPECT_TRUE(list.isFull());
  EXPECT_EQ(list.add("one too many"), TodoEditResult::ListFull);
  EXPECT_EQ(list.size(), TodoList::MAX_ITEMS);
}

TEST_F(TodoListTest, ToggleAndSetText) {
  list.add("milk");
  ASSERT_EQ(list.toggleDone(0), TodoEditResult::Ok);
  EXPECT_TRUE(list.at(0).done);
  EXPECT_EQ(list.at(0).rev, 2u);
  EXPECT_EQ(list.doneCount(), 1u);

  EXPECT_EQ(list.setText(0, " milk "), TodoEditResult::NoChange);
  EXPECT_EQ(list.setText(0, ""), TodoEditResult::EmptyText);
  ASSERT_EQ(list.setText(0, "oat milk"), TodoEditResult::Ok);
  EXPECT_EQ(list.at(0).text, "oat milk");
  EXPECT_EQ(list.at(0).rev, 3u);

  EXPECT_EQ(list.toggleDone(5), TodoEditResult::BadIndex);
  EXPECT_EQ(list.setText(5, "x"), TodoEditResult::BadIndex);
}

TEST_F(TodoListTest, RemoveLeavesTombstoneWithHigherRev) {
  list.add("a");
  list.toggleDone(0);  // rev 2
  const uint64_t id = list.at(0).id;
  ASSERT_EQ(list.remove(0), TodoEditResult::Ok);
  EXPECT_TRUE(list.empty());
  ASSERT_EQ(list.tombstones().size(), 1u);
  EXPECT_EQ(list.tombstones()[0].id, id);
  EXPECT_EQ(list.tombstones()[0].rev, 3u);
  EXPECT_TRUE(list.tombstones()[0].text.empty());
  EXPECT_EQ(list.remove(0), TodoEditResult::BadIndex);
}

TEST_F(TodoListTest, TombstonesAreCappedOldestFirst) {
  for (size_t i = 0; i < TodoList::MAX_TOMBSTONES + 5; ++i) {
    list.add("x");
    list.remove(0);
  }
  ASSERT_EQ(list.tombstones().size(), TodoList::MAX_TOMBSTONES);
  for (size_t i = 1; i < list.tombstones().size(); ++i) {
    EXPECT_LT(list.tombstones()[i - 1].seq, list.tombstones()[i].seq);
  }
}

TEST_F(TodoListTest, MoveUpAndDown) {
  list.add("a");
  list.add("b");
  list.add("c");
  EXPECT_EQ(list.moveUp(0), TodoEditResult::NoChange);
  EXPECT_EQ(list.moveDown(2), TodoEditResult::NoChange);
  ASSERT_EQ(list.moveDown(0), TodoEditResult::Ok);
  EXPECT_EQ(list.at(0).text, "b");
  EXPECT_EQ(list.at(1).text, "a");
  ASSERT_EQ(list.moveUp(2), TodoEditResult::Ok);
  EXPECT_EQ(list.at(1).text, "c");
  EXPECT_EQ(list.at(2).text, "a");
  for (size_t i = 1; i < list.size(); ++i) EXPECT_LT(list.at(i - 1).order, list.at(i).order);
}

TEST_F(TodoListTest, RemoveDone) {
  list.add("a");
  list.add("b");
  list.add("c");
  list.toggleDone(0);
  list.toggleDone(2);
  EXPECT_EQ(list.removeDone(), 2u);
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list.at(0).text, "b");
  EXPECT_EQ(list.tombstones().size(), 2u);
  EXPECT_EQ(list.removeDone(), 0u);
}

TEST_F(TodoListTest, AddRenumbersBeforeOrderOverflow) {
  list.beginLoad(10, 1, 0);
  ASSERT_TRUE(list.loadItem(makeItem(1, "last", std::numeric_limits<int32_t>::max() - 1)));
  list.endLoad();
  ASSERT_EQ(list.add("next"), TodoEditResult::Ok);
  EXPECT_LT(list.at(0).order, list.at(1).order);
  EXPECT_EQ(list.at(1).text, "next");
}

// --- loading ---

TEST_F(TodoListTest, LoadSortsByOrderThenIdAndTracksRevision) {
  list.beginLoad(5, 3, 0);
  EXPECT_TRUE(list.loadItem(makeItem(3, "third", 300, 1, 9)));
  EXPECT_TRUE(list.loadItem(makeItem(2, "tie-b", 100)));
  EXPECT_TRUE(list.loadItem(makeItem(1, "tie-a", 100)));
  list.endLoad();
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(0).text, "tie-a");
  EXPECT_EQ(list.at(1).text, "tie-b");
  EXPECT_EQ(list.at(2).text, "third");
  EXPECT_EQ(list.revision(), 9u);  // Raised to the highest seq seen.
}

TEST_F(TodoListTest, LoadRejectsInvalidEntries) {
  list.beginLoad(0, 4, 2);
  EXPECT_FALSE(list.loadItem(makeItem(0, "zero id", 1)));
  EXPECT_TRUE(list.loadItem(makeItem(1, "ok", 1)));
  EXPECT_FALSE(list.loadItem(makeItem(1, "duplicate", 2)));
  EXPECT_FALSE(list.loadItem(makeItem(2, " \n ", 3)));
  EXPECT_FALSE(list.loadTombstone(makeItem(1, "", 0)));  // Collides with a live id.
  EXPECT_TRUE(list.loadTombstone(makeItem(9, "ignored", 0)));
  list.endLoad();
  EXPECT_EQ(list.size(), 1u);
  ASSERT_EQ(list.tombstones().size(), 1u);
  EXPECT_TRUE(list.tombstones()[0].text.empty());
}

TEST_F(TodoListTest, MoveWithEqualOrdersRenumbersFirst) {
  list.beginLoad(0, 2, 0);
  list.loadItem(makeItem(1, "a", 500));
  list.loadItem(makeItem(2, "b", 500));
  list.endLoad();
  ASSERT_EQ(list.moveDown(0), TodoEditResult::Ok);
  EXPECT_EQ(list.at(0).text, "b");
  EXPECT_LT(list.at(0).order, list.at(1).order);
}

TEST_F(TodoListTest, ClearReleasesEverything) {
  list.add("a");
  list.add("b");
  list.remove(0);
  list.clear();
  EXPECT_TRUE(list.empty());
  EXPECT_TRUE(list.tombstones().empty());
  EXPECT_EQ(list.revision(), 0u);
  EXPECT_EQ(list.liveItems().capacity(), 0u);
}

}  // namespace
