// Tests for data block encoding (design doc: 4.3, U-04).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lsm/comparator.h"
#include "sstable/block.h"
#include "sstable/block_builder.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsm {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

std::shared_ptr<const Block> BuildBlock(const Entries& entries,
                                        int restart_interval) {
  BlockBuilder builder(restart_interval, BytewiseComparator());
  for (const auto& [k, v] : entries) {
    builder.Add(k, v);
  }
  return std::make_shared<Block>(std::string(builder.Finish()));
}

Entries ReadAllEntries(const std::shared_ptr<const Block>& block) {
  Entries out;
  auto it = NewBlockIterator(block, BytewiseComparator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  EXPECT_OK(it->status());
  return out;
}

Entries SortedRandomEntries(Random* rnd, size_t n) {
  std::map<std::string, std::string> m;
  while (m.size() < n) {
    m[test::RandomString(rnd, 1 + rnd->Uniform(20))] =
        test::RandomString(rnd, rnd->Uniform(30));
  }
  return Entries(m.begin(), m.end());
}

TEST(BlockTest, EmptyBlock) {
  auto block = BuildBlock({}, 16);
  ASSERT_TRUE(block->ok());
  EXPECT_EQ(block->size(), 8u);  // one restart offset + restart count
  auto it = NewBlockIterator(block, BytewiseComparator());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST(BlockTest, RoundTripForVariousRestartIntervals) {
  Random rnd(11);
  const Entries entries = SortedRandomEntries(&rnd, 500);
  for (int interval : {1, 2, 16, 1000}) {
    auto block = BuildBlock(entries, interval);
    ASSERT_TRUE(block->ok());
    const uint32_t expected_restarts =
        static_cast<uint32_t>((entries.size() + interval - 1) / interval);
    EXPECT_EQ(block->num_restarts(), expected_restarts);
    EXPECT_EQ(ReadAllEntries(block), entries) << "interval " << interval;
  }
}

TEST(BlockTest, PrefixCompressionShrinksSharedPrefixes) {
  Entries entries;
  for (int i = 0; i < 200; ++i) {
    entries.emplace_back("user/profile/" + test::NumberKey(i), "v");
  }
  size_t raw = 0;
  for (const auto& [k, v] : entries) {
    raw += k.size() + v.size();
  }
  auto compressed = BuildBlock(entries, 16);
  auto uncompressed = BuildBlock(entries, 1);  // every key stored in full
  EXPECT_LT(compressed->size(), uncompressed->size() * 6 / 10);
  EXPECT_LT(compressed->size(), raw);
  EXPECT_EQ(ReadAllEntries(compressed), entries);
}

TEST(BlockTest, SeekUsesBinarySearchOverRestarts) {
  Entries entries;
  for (int i = 0; i < 1000; i += 2) {  // even numbers only
    entries.emplace_back(test::NumberKey(i), "v" + std::to_string(i));
  }
  auto block = BuildBlock(entries, 16);
  auto it = NewBlockIterator(block, BytewiseComparator());
  for (int i = 0; i < 1000; ++i) {
    it->Seek(test::NumberKey(i));
    const int expected = (i % 2 == 0) ? i : i + 1;
    if (expected >= 1000) {
      EXPECT_FALSE(it->Valid());
      continue;
    }
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(it->key(), test::NumberKey(expected));
    EXPECT_EQ(it->value(), "v" + std::to_string(expected));
  }
  it->Seek("");  // before everything
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), test::NumberKey(0));
  it->Seek("zzz");  // after everything
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST(BlockTest, MalformedRestartArrayIsRejected) {
  EXPECT_FALSE(Block(std::string()).ok());          // too short
  EXPECT_FALSE(Block(std::string("\x01\x00", 2)).ok());
  std::string zero_restarts;
  PutFixed32(&zero_restarts, 0);
  EXPECT_FALSE(Block(zero_restarts).ok());
  std::string too_many;
  PutFixed32(&too_many, 1000);  // claims 1000 restarts in a 4-byte block
  EXPECT_FALSE(Block(too_many).ok());

  auto it = NewBlockIterator(std::make_shared<Block>(too_many),
                             BytewiseComparator());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().is_corruption());
}

TEST(BlockTest, CorruptedEntriesYieldCorruptionNotCrash) {
  Random rnd(5);
  const Entries entries = SortedRandomEntries(&rnd, 100);
  BlockBuilder builder(4, BytewiseComparator());
  for (const auto& [k, v] : entries) {
    builder.Add(k, v);
  }
  const std::string good(builder.Finish());

  int corruptions = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    std::string bad = good;
    const size_t pos = rnd.Uniform(bad.size());
    bad[pos] = static_cast<char>(rnd.Uniform(256));
    auto block = std::make_shared<Block>(bad);
    auto it = NewBlockIterator(block, BytewiseComparator());
    size_t n = 0;
    for (it->SeekToFirst(); it->Valid() && n < 10 * entries.size(); it->Next()) {
      ++n;
    }
    it->Seek(entries[rnd.Uniform(entries.size())].first);
    corruptions += it->status().is_corruption() ? 1 : 0;
  }
  // Many (not all) random byte changes are detectable from structure alone;
  // the important property is that none of them crashed.
  EXPECT_GT(corruptions, 0);
}

}  // namespace
}  // namespace lsm
