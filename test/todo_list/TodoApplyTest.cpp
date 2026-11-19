#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "lib/TodoList/TodoList.h"

// applyIncoming() implements docs/todo-sync-protocol.md C5 steps 2-6.

namespace {

uint32_t fixedRng() { return 1; }

TodoItem live(uint64_t id, const std::string& text, int32_t order, uint32_t rev = 1, bool done = false) {
  TodoItem item;
  item.id = id;
  item.text = text;
  item.order = order;
  item.rev = rev;
  item.done = done;
  return item;
}

TodoItem tomb(uint64_t id, uint32_t rev = 1) {
  TodoItem item;
  item.id = id;
  item.rev = rev;
  return item;
}

class TodoApplyTest : public ::testing::Test {
 protected:
  // Device state: list revision 10; A (seq 3) and B (seq 7) live, T (seq 9) deleted.
  void SetUp() override {
    list.beginLoad(10, 2, 1);
    TodoItem a = live(0xA, "alpha", 1024, 2);
    a.seq = 3;
    TodoItem b = live(0xB, "bravo", 2048, 4, true);
    b.seq = 7;
    TodoItem t = tomb(0xF, 3);
    t.seq = 9;
    ASSERT_TRUE(list.loadItem(a));
    ASSERT_TRUE(list.loadItem(b));
    ASSERT_TRUE(list.loadTombstone(t));
    list.endLoad();
    ASSERT_EQ(list.revision(), 10u);
  }

  // The phone's view of exactly the stored state.
  TodoIncoming same() const {
    TodoIncoming in;
    in.baseRev = 10;
    in.items = {live(0xA, "alpha", 1024, 2), live(0xB, "bravo", 2048, 4, true)};
    in.deleted = {tomb(0xF, 3)};
    return in;
  }

  // The list must be exactly as SetUp left it.
  void expectUntouched() const {
    EXPECT_EQ(list.revision(), 10u);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list.at(0).id, 0xAu);
    EXPECT_EQ(list.at(0).text, "alpha");
    EXPECT_EQ(list.at(0).seq, 3u);
    EXPECT_EQ(list.at(1).id, 0xBu);
    EXPECT_TRUE(list.at(1).done);
    EXPECT_EQ(list.at(1).seq, 7u);
    ASSERT_EQ(list.tombstones().size(), 1u);
    EXPECT_EQ(list.tombstones()[0].seq, 9u);
  }

  TodoList list{&fixedRng};
};

TEST_F(TodoApplyTest, StaleBaseRevIsAConflictAndChangesNothing) {
  TodoIncoming in = same();
  in.baseRev = 9;
  in.items[0].text = "changed";
  const auto result = list.applyIncoming(std::move(in));
  EXPECT_EQ(result.status, TodoApplyStatus::Conflict);
  expectUntouched();
}

TEST_F(TodoApplyTest, ConflictIsCheckedBeforeValidation) {
  TodoIncoming in = same();
  in.baseRev = 99;
  in.items[0].id = 0;  // would be invalid, but the stale base wins
  EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Conflict);
  expectUntouched();
}

TEST_F(TodoApplyTest, DuplicateIdIsRejected) {
  TodoIncoming in = same();
  in.items[1].id = in.items[0].id;
  const auto result = list.applyIncoming(std::move(in));
  EXPECT_EQ(result.status, TodoApplyStatus::Invalid);
  EXPECT_STREQ(result.reason, "duplicate id");
  expectUntouched();
}

TEST_F(TodoApplyTest, DuplicateAcrossLiveAndDeletedIsRejected) {
  TodoIncoming in = same();
  in.deleted[0].id = in.items[0].id;
  EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Invalid);
  expectUntouched();
}

TEST_F(TodoApplyTest, ZeroIdIsRejected) {
  TodoIncoming in = same();
  in.items[0].id = 0;
  EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Invalid);
  expectUntouched();
}

TEST_F(TodoApplyTest, EmptyTextIsRejected) {
  TodoIncoming in = same();
  in.items[0].text = " \n\t ";
  const auto result = list.applyIncoming(std::move(in));
  EXPECT_EQ(result.status, TodoApplyStatus::Invalid);
  EXPECT_STREQ(result.reason, "empty text");
  expectUntouched();
}

TEST_F(TodoApplyTest, MoreThan100LiveItemsIsRejected) {
  TodoIncoming in = same();
  for (uint64_t i = 0; in.items.size() < TodoList::MAX_ITEMS + 1; ++i)
    in.items.push_back(live(0x1000 + i, "x", 5000 + i));
  const auto result = list.applyIncoming(std::move(in));
  EXPECT_EQ(result.status, TodoApplyStatus::Invalid);
  EXPECT_STREQ(result.reason, "too many items");
  expectUntouched();
}

TEST_F(TodoApplyTest, MoreThan64TombstonesIsRejected) {
  TodoIncoming in = same();
  for (uint64_t i = 0; in.deleted.size() < TodoList::MAX_TOMBSTONES + 1; ++i) in.deleted.push_back(tomb(0x2000 + i));
  const auto result = list.applyIncoming(std::move(in));
  EXPECT_EQ(result.status, TodoApplyStatus::Invalid);
  EXPECT_STREQ(result.reason, "too many tombstones");
  expectUntouched();
}

TEST_F(TodoApplyTest, OmittingALiveIdIsRejectedAsMissing) {
  TodoIncoming in = same();
  in.items.erase(in.items.begin());  // phone forgot A
  const auto result = list.applyIncoming(std::move(in));
  EXPECT_EQ(result.status, TodoApplyStatus::Invalid);
  EXPECT_STREQ(result.reason, "missing id");
  expectUntouched();
}

TEST_F(TodoApplyTest, IdenticalRecordsAreUnchangedWithNoRevisionBump) {
  EXPECT_EQ(list.applyIncoming(same()).status, TodoApplyStatus::Unchanged);
  expectUntouched();
  // A retry of the same body stays unchanged.
  EXPECT_EQ(list.applyIncoming(same()).status, TodoApplyStatus::Unchanged);
  expectUntouched();
}

TEST_F(TodoApplyTest, SanitizedTextEqualToStoredIsStillUnchanged) {
  TodoIncoming in = same();
  in.items[0].text = "  alpha \n";
  EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Unchanged);
  expectUntouched();
}

TEST_F(TodoApplyTest, NewAndDeletedRecordsGetSeqOthersKeepIt) {
  TodoIncoming in;
  in.baseRev = 10;
  in.items = {
      live(0xA, "alpha", 1024, 2),    // unchanged -> keeps seq 3
      live(0xC, "charlie", 3072, 1),  // new
  };
  in.deleted = {tomb(0xB, 5), tomb(0xF, 3)};  // B deleted by the phone (changed); F unchanged -> keeps seq 9
  const auto result = list.applyIncoming(std::move(in));
  ASSERT_EQ(result.status, TodoApplyStatus::Applied);

  // Two changed records (C new, B deleted) -> revision 10 + 2.
  EXPECT_EQ(list.revision(), 12u);
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list.at(0).id, 0xAu);
  EXPECT_EQ(list.at(0).seq, 3u);
  EXPECT_EQ(list.at(1).id, 0xCu);
  EXPECT_EQ(list.at(1).seq, 11u);  // request order: live records first
  ASSERT_EQ(list.tombstones().size(), 2u);
  EXPECT_EQ(list.tombstones()[0].id, 0xFu);  // oldest seq first
  EXPECT_EQ(list.tombstones()[0].seq, 9u);
  EXPECT_EQ(list.tombstones()[1].id, 0xBu);
  EXPECT_EQ(list.tombstones()[1].seq, 12u);
  EXPECT_EQ(list.tombstones()[1].rev, 5u);
  EXPECT_TRUE(list.tombstones()[1].text.empty());
}

TEST_F(TodoApplyTest, EditedRecordTakesThePhonesRevAndNewSeq) {
  TodoIncoming in = same();
  in.items[0].text = "alpha edited";
  in.items[0].rev = 3;
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  EXPECT_EQ(list.revision(), 11u);
  EXPECT_EQ(list.at(0).text, "alpha edited");
  EXPECT_EQ(list.at(0).rev, 3u);
  EXPECT_EQ(list.at(0).seq, 11u);
  EXPECT_EQ(list.at(1).seq, 7u);  // B untouched
}

TEST_F(TodoApplyTest, StoredTextIsSanitized) {
  TodoIncoming in = same();
  in.items.push_back(live(0xD, "  buy\n\tmilk   now ", 4096));
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(2).text, "buy milk now");
}

TEST_F(TodoApplyTest, DisplayOrderTiesAreBrokenById) {
  TodoIncoming in;
  in.baseRev = 10;
  in.items = {live(0xB, "bravo", 500, 4, true), live(0xA, "alpha", 500, 2), live(0xC, "charlie", 100)};
  in.deleted = {tomb(0xF, 3)};
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(0).id, 0xCu);  // lowest order
  EXPECT_EQ(list.at(1).id, 0xAu);  // tie at 500: lower id first
  EXPECT_EQ(list.at(2).id, 0xBu);
}

TEST_F(TodoApplyTest, AppliedStateIsStableOnRetry) {
  TodoIncoming in = same();
  in.items.push_back(live(0xD, "delta", 4096));
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  const uint32_t rev = list.revision();

  // The phone adopts the response and (after a lost reply) resends its request with the new base.
  TodoIncoming retry;
  retry.baseRev = rev;
  for (const auto& item : list.liveItems()) retry.items.push_back(item);
  for (const auto& item : list.tombstones()) retry.deleted.push_back(item);
  EXPECT_EQ(list.applyIncoming(std::move(retry)).status, TodoApplyStatus::Unchanged);
  EXPECT_EQ(list.revision(), rev);
}

// Each of these is a real change, so the list is replaced and the revision moves.
TEST_F(TodoApplyTest, AnyFieldChangeIsApplied) {
  {
    TodoIncoming in = same();
    in.items[0].done = true;
    EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  }
  {
    TodoIncoming in;
    in.baseRev = list.revision();
    for (const auto& item : list.liveItems()) in.items.push_back(item);
    for (const auto& item : list.tombstones()) in.deleted.push_back(item);
    in.items[1].order += 7;  // order only
    EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  }
  {
    TodoIncoming in;
    in.baseRev = list.revision();
    for (const auto& item : list.liveItems()) in.items.push_back(item);
    for (const auto& item : list.tombstones()) in.deleted.push_back(item);
    in.items[0].rev += 1;  // rev only
    EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  }
}

TEST_F(TodoApplyTest, ChangedTombstoneRevGetsANewSeq) {
  TodoIncoming in = same();
  in.deleted[0].rev = 8;
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  EXPECT_EQ(list.revision(), 11u);
  ASSERT_EQ(list.tombstones().size(), 1u);
  EXPECT_EQ(list.tombstones()[0].rev, 8u);
  EXPECT_EQ(list.tombstones()[0].seq, 11u);
}

TEST_F(TodoApplyTest, TombstoneFieldsAreCleared) {
  TodoIncoming in = same();
  in.deleted[0].rev = 8;
  in.deleted[0].text = "leftover";
  in.deleted[0].done = true;
  in.deleted[0].order = 99;
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  const TodoItem& t = list.tombstones()[0];
  EXPECT_TRUE(t.text.empty());
  EXPECT_FALSE(t.done);
  EXPECT_EQ(t.order, 0);
}

TEST_F(TodoApplyTest, LiveIdMatchingAStoredTombstoneIsAResurrection) {
  TodoIncoming in;
  in.baseRev = 10;
  in.items = {live(0xA, "alpha", 1024, 2), live(0xB, "bravo", 2048, 4, true), live(0xF, "back again", 3072, 4)};
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(2).id, 0xFu);
  EXPECT_EQ(list.at(2).seq, 11u);
  EXPECT_TRUE(list.tombstones().empty());
}

TEST_F(TodoApplyTest, DifferentRecordCountsAreNotUnchanged) {
  TodoIncoming in = same();
  in.deleted.push_back(tomb(0x77, 2));  // phone knows one more tombstone
  EXPECT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  EXPECT_EQ(list.tombstones().size(), 2u);
}

// Contract decision (C5 step 6, "replace the list"): a stored tombstone the phone leaves out is dropped.
// The revision does not move because no record is new or changed.
TEST_F(TodoApplyTest, OmittingAStoredTombstoneReplacesTheList) {
  TodoIncoming in = same();
  in.deleted.clear();
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  EXPECT_TRUE(list.tombstones().empty());
  EXPECT_EQ(list.revision(), 10u);
}

// A stored text never keeps more than a sanitized block (the 120-byte limit plus one reserved byte), however
// large the raw text was. (std::string move-assign adopts the sanitized buffer; this pins that behavior.)
TEST_F(TodoApplyTest, StoredTextNeverKeepsAnOversizedBuffer) {
  TodoIncoming in = same();
  in.items.push_back(live(0xD, "ok" + std::string(300, ' '), 4096));
  ASSERT_EQ(list.applyIncoming(std::move(in)).status, TodoApplyStatus::Applied);
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list.at(2).text, "ok");
  EXPECT_LE(list.at(2).text.capacity(), TodoList::MAX_TEXT_BYTES + 16);  // slack: libc++ rounds capacities up
}

}  // namespace
