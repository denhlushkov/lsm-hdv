// Tests for the MemTable (design doc, 2.1.2).
#include <map>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "db/dbformat.h"
#include "memtable/memtable.h"
#include "test_util.h"

namespace lsm {
namespace {

class MemTableTest : public ::testing::Test {
 protected:
  MemTableTest()
      : icmp_(BytewiseComparator()), mem_(std::make_shared<MemTable>(icmp_)) {}

  // Returns "NOT_FOUND", "DELETED" or the value.
  std::string Get(std::string_view key,
                  SequenceNumber seq = kMaxSequenceNumber) {
    std::string value;
    Status s;
    if (!mem_->Get(LookupKey(key, seq), &value, &s)) {
      return "NOT_FOUND";
    }
    return s.is_not_found() ? "DELETED" : value;
  }

  InternalKeyComparator icmp_;
  std::shared_ptr<MemTable> mem_;
};

TEST_F(MemTableTest, PutAndGet) {
  ASSERT_OK(mem_->Put(1, "apple", "red"));
  ASSERT_OK(mem_->Put(2, "banana", "yellow"));
  EXPECT_EQ(Get("apple"), "red");
  EXPECT_EQ(Get("banana"), "yellow");
  EXPECT_EQ(Get("cherry"), "NOT_FOUND");
  EXPECT_EQ(Get("appl"), "NOT_FOUND");
  EXPECT_EQ(Get("apple1"), "NOT_FOUND");
  EXPECT_EQ(mem_->NumEntries(), 2u);
}

TEST_F(MemTableTest, EmptyKeyAndValueAreSupported) {
  ASSERT_OK(mem_->Put(1, "", "empty-key"));
  ASSERT_OK(mem_->Put(2, "k", ""));
  EXPECT_EQ(Get(""), "empty-key");
  EXPECT_EQ(Get("k"), "");
}

TEST_F(MemTableTest, NewerVersionShadowsOlder) {
  ASSERT_OK(mem_->Put(1, "k", "v1"));
  ASSERT_OK(mem_->Put(2, "k", "v2"));
  ASSERT_OK(mem_->Put(3, "k", "v3"));
  EXPECT_EQ(Get("k"), "v3");
  EXPECT_EQ(mem_->NumEntries(), 3u);  // versions are kept, not overwritten
}

TEST_F(MemTableTest, DeleteWritesTombstone) {
  ASSERT_OK(mem_->Put(1, "k", "v1"));
  ASSERT_OK(mem_->Delete(2, "k"));
  // The tombstone answers the lookup: older data must not be consulted.
  EXPECT_EQ(Get("k"), "DELETED");
  ASSERT_OK(mem_->Put(3, "k", "v3"));
  EXPECT_EQ(Get("k"), "v3");
}

TEST_F(MemTableTest, LookupAtOlderSequenceSeesOlderVersion) {
  ASSERT_OK(mem_->Put(10, "k", "v10"));
  ASSERT_OK(mem_->Delete(20, "k"));
  ASSERT_OK(mem_->Put(30, "k", "v30"));
  EXPECT_EQ(Get("k", 5), "NOT_FOUND");
  EXPECT_EQ(Get("k", 10), "v10");
  EXPECT_EQ(Get("k", 19), "v10");
  EXPECT_EQ(Get("k", 20), "DELETED");
  EXPECT_EQ(Get("k", 29), "DELETED");
  EXPECT_EQ(Get("k", 30), "v30");
}

TEST_F(MemTableTest, IteratorYieldsInternalKeyOrderBothWays) {
  ASSERT_OK(mem_->Put(1, "b", "b1"));
  ASSERT_OK(mem_->Put(2, "a", "a2"));
  ASSERT_OK(mem_->Put(3, "b", "b3"));
  ASSERT_OK(mem_->Delete(4, "c"));

  struct Expected {
    std::string user_key;
    SequenceNumber seq;
    ValueType type;
    std::string value;
  };
  const std::vector<Expected> expected = {
      {"a", 2, kTypeValue, "a2"},
      {"b", 3, kTypeValue, "b3"},  // newest version of "b" first
      {"b", 1, kTypeValue, "b1"},
      {"c", 4, kTypeDeletion, ""},
  };

  auto iter = mem_->NewIterator();
  iter->SeekToFirst();
  for (const auto& e : expected) {
    ASSERT_TRUE(iter->Valid());
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(iter->key(), &parsed));
    EXPECT_EQ(parsed.user_key, e.user_key);
    EXPECT_EQ(parsed.sequence, e.seq);
    EXPECT_EQ(parsed.type, e.type);
    EXPECT_EQ(iter->value(), e.value);
    iter->Next();
  }
  EXPECT_FALSE(iter->Valid());

  iter->SeekToLast();
  for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
    ASSERT_TRUE(iter->Valid());
    EXPECT_EQ(ExtractUserKey(iter->key()), it->user_key);
    iter->Prev();
  }
  EXPECT_FALSE(iter->Valid());

  iter->Seek(MakeInternalKey("b", kMaxSequenceNumber, kValueTypeForSeek));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->value(), "b3");
  iter->SeekForPrev(MakeInternalKey("bb", 0, kTypeValue));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->value(), "b1");
}

TEST_F(MemTableTest, IteratorKeepsMemTableAlive) {
  ASSERT_OK(mem_->Put(1, "k", "v"));
  auto iter = mem_->NewIterator();
  mem_.reset();  // drop the only external reference
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->value(), "v");
}

TEST_F(MemTableTest, MemoryUsageDrivesFlushPolicy) {
  constexpr size_t kWriteBuffer = 64 * 1024;
  const size_t initial = mem_->ApproximateMemoryUsage();
  EXPECT_FALSE(mem_->ShouldFlush(kWriteBuffer));
  SequenceNumber seq = 1;
  const std::string value(100, 'v');
  while (!mem_->ShouldFlush(kWriteBuffer)) {
    ASSERT_OK(mem_->Put(seq, test::NumberKey(seq), value));
    ++seq;
    ASSERT_LT(seq, 10000u) << "memory usage does not grow";
  }
  EXPECT_GT(mem_->ApproximateMemoryUsage(), initial);
  // ~120 bytes per entry: the threshold is reached after a bounded number
  // of writes.
  EXPECT_GT(seq, kWriteBuffer / 200);
}

TEST_F(MemTableTest, ImmutableTableRejectsWrites) {
  ASSERT_OK(mem_->Put(1, "k", "v"));
  mem_->MarkImmutable();
  EXPECT_TRUE(mem_->IsImmutable());
  EXPECT_TRUE(mem_->Put(2, "k2", "v2").is_invalid_argument());
  EXPECT_EQ(Get("k"), "v");  // reads still work
  EXPECT_EQ(Get("k2"), "NOT_FOUND");
}

// Differential check against std::map (design doc, D-01 at MemTable level).
TEST_F(MemTableTest, RandomOperationsMatchStdMap) {
  Random rnd(2024);
  std::map<std::string, std::string> model;
  SequenceNumber seq = 1;
  for (int i = 0; i < 20000; ++i) {
    const std::string key = "k" + std::to_string(rnd.Uniform(500));
    if (rnd.OneIn(4)) {
      ASSERT_OK(mem_->Delete(seq++, key));
      model.erase(key);
    } else {
      const std::string value = test::RandomString(&rnd, rnd.Uniform(40));
      ASSERT_OK(mem_->Put(seq++, key, value));
      model[key] = value;
    }
  }
  for (int k = 0; k < 500; ++k) {
    const std::string key = "k" + std::to_string(k);
    const auto it = model.find(key);
    const std::string got = Get(key);
    if (it == model.end()) {
      EXPECT_TRUE(got == "DELETED" || got == "NOT_FOUND") << key;
    } else {
      EXPECT_EQ(got, it->second) << key;
    }
  }
}

}  // namespace
}  // namespace lsm
