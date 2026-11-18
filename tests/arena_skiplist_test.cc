// Tests for the Arena and the SkipList (design doc, U-01 and section 4.2.2).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "memtable/skiplist.h"
#include "test_util.h"
#include "util/arena.h"
#include "util/random.h"

namespace lsm {
namespace {

using Key = uint64_t;

struct IntComparator {
  int operator()(const Key& a, const Key& b) const {
    return a < b ? -1 : (a > b ? 1 : 0);
  }
};

using IntSkipList = SkipList<Key, IntComparator>;

// ------------------------------------------------------------------ Arena

TEST(ArenaTest, AllocationsAreDistinctAndWritable) {
  Arena arena;
  Random rnd(301);
  std::vector<std::pair<size_t, char*>> allocated;
  size_t total = 0;
  for (int i = 0; i < 2000; ++i) {
    size_t n = (i % 100 == 0) ? 6000 : 1 + rnd.Uniform(i % 20 == 0 ? 2000 : 20);
    char* p = (i % 2 == 0) ? arena.AllocateAligned(n) : arena.Allocate(n);
    for (size_t b = 0; b < n; ++b) {
      p[b] = static_cast<char>(i % 256);  // fill with a known pattern
    }
    allocated.emplace_back(n, p);
    total += n;
    EXPECT_GE(arena.MemoryUsage(), total);
  }
  // No allocation overwrote another one.
  for (size_t i = 0; i < allocated.size(); ++i) {
    const auto [n, p] = allocated[i];
    for (size_t b = 0; b < n; ++b) {
      ASSERT_EQ(static_cast<unsigned char>(p[b]), i % 256);
    }
  }
}

TEST(ArenaTest, AlignedAllocationsAreAligned) {
  Arena arena;
  arena.Allocate(1);  // misalign the bump pointer
  for (int i = 0; i < 100; ++i) {
    char* p = arena.AllocateAligned(static_cast<size_t>(1 + i % 13));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u);
    arena.Allocate(static_cast<size_t>(1 + i % 7));
  }
}

TEST(ArenaTest, LargeAllocationsGetDedicatedBlocks) {
  Arena arena;
  arena.Allocate(10);
  const size_t before = arena.MemoryUsage();
  arena.Allocate(100000);
  EXPECT_GE(arena.MemoryUsage() - before, 100000u);
  EXPECT_LT(arena.MemoryUsage() - before, 100000u + Arena::kBlockSize);
}

// --------------------------------------------------------------- SkipList

TEST(SkipListTest, EmptyList) {
  Arena arena;
  IntSkipList list(IntComparator(), &arena);
  EXPECT_FALSE(list.Contains(10));
  EXPECT_EQ(list.Size(), 0u);
  IntSkipList::Iterator iter(&list);
  EXPECT_FALSE(iter.Valid());
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek(100);
  EXPECT_FALSE(iter.Valid());
  iter.SeekToLast();
  EXPECT_FALSE(iter.Valid());
}

// U-01: insert/search on 100 000 random keys, iterator order.
TEST(SkipListTest, InsertAndLookup100kRandomKeys) {
  constexpr int kN = 100000;
  constexpr Key kRange = 1000000;
  Random rnd(1000);
  std::set<Key> model;
  Arena arena;
  IntSkipList list(IntComparator(), &arena);
  for (int i = 0; i < kN; ++i) {
    const Key key = rnd.Uniform(kRange);
    if (model.insert(key).second) {
      list.Insert(key);
    }
  }
  EXPECT_EQ(list.Size(), model.size());

  for (Key i = 0; i < kRange; i += 7) {
    ASSERT_EQ(list.Contains(i), model.count(i) == 1) << i;
  }

  // Forward iteration matches the model exactly.
  IntSkipList::Iterator iter(&list);
  iter.SeekToFirst();
  for (Key expected : model) {
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(iter.key(), expected);
    iter.Next();
  }
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipListTest, SeekFindsFirstGreaterOrEqual) {
  Arena arena;
  IntSkipList list(IntComparator(), &arena);
  for (Key k = 10; k <= 100; k += 10) {
    list.Insert(k);
  }
  IntSkipList::Iterator iter(&list);
  iter.Seek(0);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 10u);
  iter.Seek(10);
  EXPECT_EQ(iter.key(), 10u);
  iter.Seek(11);
  EXPECT_EQ(iter.key(), 20u);
  iter.Seek(100);
  EXPECT_EQ(iter.key(), 100u);
  iter.Seek(101);
  EXPECT_FALSE(iter.Valid());

  iter.SeekForPrev(55);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 50u);
  iter.SeekForPrev(60);
  EXPECT_EQ(iter.key(), 60u);
  iter.SeekForPrev(5);
  EXPECT_FALSE(iter.Valid());
  iter.SeekForPrev(1000);
  EXPECT_EQ(iter.key(), 100u);
}

TEST(SkipListTest, BackwardIteration) {
  Random rnd(7);
  Arena arena;
  IntSkipList list(IntComparator(), &arena);
  std::set<Key> model;
  for (int i = 0; i < 5000; ++i) {
    const Key k = rnd.Uniform(100000);
    if (model.insert(k).second) {
      list.Insert(k);
    }
  }
  IntSkipList::Iterator iter(&list);
  iter.SeekToLast();
  for (auto it = model.rbegin(); it != model.rend(); ++it) {
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(iter.key(), *it);
    iter.Prev();
  }
  EXPECT_FALSE(iter.Valid());
}

// Section 4.2.2: P(height = h) = (1 - p) p^(h - 1) with p = 1/4.
TEST(SkipListTest, HeightDistributionIsGeometric) {
  constexpr int kSamples = 400000;
  Random rnd(42);
  std::vector<int> counts(kSkipListMaxHeight + 1, 0);
  for (int i = 0; i < kSamples; ++i) {
    const int h = RandomSkipListHeight(&rnd);
    ASSERT_GE(h, 1);
    ASSERT_LE(h, kSkipListMaxHeight);
    ++counts[h];
  }
  const double p = 1.0 / kSkipListBranching;
  for (int h = 1; h <= 5; ++h) {
    const double expected = (1 - p) * std::pow(p, h - 1);
    const double observed = static_cast<double>(counts[h]) / kSamples;
    // Allow 5 standard deviations of sampling error.
    const double sigma = std::sqrt(expected * (1 - expected) / kSamples);
    EXPECT_NEAR(observed, expected, 5 * sigma + 1e-4) << "height " << h;
  }
}

TEST(SkipListTest, MaxHeightGrowsLogarithmically) {
  Arena arena;
  IntSkipList list(IntComparator(), &arena);
  for (Key k = 0; k < 50000; ++k) {
    list.Insert(k);
  }
  // log_4(50000) ~ 7.8; the height must be in a sane range.
  EXPECT_GE(list.CurrentMaxHeight(), 5);
  EXPECT_LE(list.CurrentMaxHeight(), kSkipListMaxHeight);
}

// One writer inserting while several readers iterate: readers must always
// see a strictly increasing sequence and never crash (design doc, 2.3).
TEST(SkipListTest, ConcurrentReadersWithSingleWriter) {
  constexpr Key kKeys = 20000;
  Arena arena;
  IntSkipList list(IntComparator(), &arena);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> scans{0};
  std::atomic<bool> order_violation{false};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!done.load(std::memory_order_acquire)) {
        IntSkipList::Iterator iter(&list);
        Key prev = 0;
        bool first = true;
        for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
          if (!first && iter.key() <= prev) {
            order_violation = true;
          }
          prev = iter.key();
          first = false;
        }
        scans.fetch_add(1);
      }
    });
  }

  Random rnd(99);
  std::set<Key> inserted;
  while (inserted.size() < kKeys) {
    const Key k = rnd.Uniform(kKeys * 10);
    if (inserted.insert(k).second) {
      list.Insert(k);
    }
  }
  done.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }
  EXPECT_FALSE(order_violation.load());
  EXPECT_GT(scans.load(), 0u);
  EXPECT_EQ(list.Size(), inserted.size());
}

}  // namespace
}  // namespace lsm
