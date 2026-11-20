// Tests for the LRU block cache (design doc, U-03).
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cache/lru_cache.h"
#include "test_util.h"

namespace lsm {
namespace {

std::shared_ptr<void> Value(int v) { return std::make_shared<int>(v); }

int Get(LRUCache& cache, const std::string& key) {
  auto v = cache.LookupAs<int>(key);
  return v ? *v : -1;
}

TEST(LRUCacheTest, InsertAndLookup) {
  LRUCache cache(1000, 0);
  EXPECT_EQ(Get(cache, "a"), -1);
  cache.Insert("a", Value(1), 1);
  cache.Insert("b", Value(2), 1);
  EXPECT_EQ(Get(cache, "a"), 1);
  EXPECT_EQ(Get(cache, "b"), 2);
  EXPECT_EQ(cache.EntryCount(), 2u);
  EXPECT_EQ(cache.TotalCharge(), 2u);

  const CacheStats stats = cache.GetStats();
  EXPECT_EQ(stats.hits, 2u);
  EXPECT_EQ(stats.misses, 1u);
  EXPECT_EQ(stats.inserts, 2u);
}

TEST(LRUCacheTest, InsertReplacesExistingEntry) {
  LRUCache cache(1000, 0);
  cache.Insert("k", Value(1), 10);
  cache.Insert("k", Value(2), 30);
  EXPECT_EQ(Get(cache, "k"), 2);
  EXPECT_EQ(cache.EntryCount(), 1u);
  EXPECT_EQ(cache.TotalCharge(), 30u);  // old charge released
}

// U-03: least recently used entries are evicted when over capacity.
TEST(LRUCacheTest, EvictsLeastRecentlyUsed) {
  LRUCache cache(3, 0);
  cache.Insert("a", Value(1), 1);
  cache.Insert("b", Value(2), 1);
  cache.Insert("c", Value(3), 1);
  cache.Insert("d", Value(4), 1);  // evicts "a"
  EXPECT_EQ(Get(cache, "a"), -1);
  EXPECT_EQ(Get(cache, "b"), 2);
  EXPECT_EQ(Get(cache, "c"), 3);
  EXPECT_EQ(Get(cache, "d"), 4);
  EXPECT_EQ(cache.GetStats().evictions, 1u);
}

// U-03: repeated access refreshes an entry's position.
TEST(LRUCacheTest, LookupRefreshesRecency) {
  LRUCache cache(3, 0);
  cache.Insert("a", Value(1), 1);
  cache.Insert("b", Value(2), 1);
  cache.Insert("c", Value(3), 1);
  EXPECT_EQ(Get(cache, "a"), 1);   // "a" becomes most recent
  cache.Insert("d", Value(4), 1);  // evicts "b", the least recent
  EXPECT_EQ(Get(cache, "a"), 1);
  EXPECT_EQ(Get(cache, "b"), -1);
  EXPECT_EQ(Get(cache, "c"), 3);
  EXPECT_EQ(Get(cache, "d"), 4);
}

TEST(LRUCacheTest, CapacityIsMeasuredInCharge) {
  LRUCache cache(100, 0);
  cache.Insert("small1", Value(1), 10);
  cache.Insert("small2", Value(2), 10);
  cache.Insert("big", Value(3), 85);  // 105 > 100: evicts small1 only
  EXPECT_EQ(Get(cache, "small1"), -1);
  EXPECT_EQ(Get(cache, "small2"), 2);
  EXPECT_EQ(Get(cache, "big"), 3);
  EXPECT_LE(cache.TotalCharge(), 100u);

  // An entry larger than the whole cache is not retained.
  cache.Insert("huge", Value(4), 1000);
  EXPECT_EQ(Get(cache, "huge"), -1);
  EXPECT_LE(cache.TotalCharge(), 100u);
}

TEST(LRUCacheTest, EraseAndClear) {
  LRUCache cache(100, 0);
  cache.Insert("a", Value(1), 5);
  cache.Insert("b", Value(2), 5);
  EXPECT_TRUE(cache.Erase("a"));
  EXPECT_FALSE(cache.Erase("a"));
  EXPECT_EQ(Get(cache, "a"), -1);
  EXPECT_EQ(cache.TotalCharge(), 5u);
  cache.Clear();
  EXPECT_EQ(cache.EntryCount(), 0u);
  EXPECT_EQ(cache.TotalCharge(), 0u);
  EXPECT_EQ(Get(cache, "b"), -1);
}

TEST(LRUCacheTest, ZeroCapacityCachesNothing) {
  LRUCache cache(0, 0);
  cache.Insert("a", Value(1), 1);
  EXPECT_EQ(Get(cache, "a"), -1);
  EXPECT_EQ(cache.EntryCount(), 0u);
}

TEST(LRUCacheTest, EvictedValueStaysAliveWhileReferenced) {
  LRUCache cache(1, 0);
  auto value = std::make_shared<std::string>("block contents");
  std::weak_ptr<std::string> weak = value;
  cache.Insert("a", value, 1);
  auto held = cache.LookupAs<std::string>("a");
  value.reset();
  cache.Insert("b", Value(2), 1);  // evicts "a"
  EXPECT_EQ(Get(cache, "a"), -1);
  ASSERT_FALSE(weak.expired());  // still referenced by `held`
  EXPECT_EQ(*held, "block contents");
  held.reset();
  EXPECT_TRUE(weak.expired());  // freed with the last reference
}

TEST(LRUCacheTest, NewIdIsUnique) {
  LRUCache cache(10);
  const uint64_t a = cache.NewId();
  const uint64_t b = cache.NewId();
  EXPECT_NE(a, b);
}

TEST(LRUCacheTest, ShardedCacheUnderConcurrentAccess) {
  LRUCache cache(4096, 4);
  std::vector<std::thread> threads;
  std::atomic<int> wrong_values{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      Random rnd(static_cast<uint64_t>(t) + 1);
      for (int i = 0; i < 20000; ++i) {
        const int k = static_cast<int>(rnd.Uniform(1000));
        const std::string key = "key" + std::to_string(k);
        if (rnd.OneIn(3)) {
          cache.Insert(key, Value(k), 8);
        } else if (auto v = cache.LookupAs<int>(key)) {
          if (*v != k) {
            ++wrong_values;
          }
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  EXPECT_EQ(wrong_values.load(), 0);
  // Each of the 16 shards holds at most ceil(4096 / 16) bytes.
  EXPECT_LE(cache.TotalCharge(), 4096u);
  const CacheStats stats = cache.GetStats();
  EXPECT_GT(stats.hits, 0u);
  EXPECT_GT(stats.evictions, 0u);
}

}  // namespace
}  // namespace lsm
