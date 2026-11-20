// LRU block cache (design doc, section 2.1.3): a hash table for O(1)
// lookup combined with an intrusive doubly linked list ordered by recency.
//
// Capacity is measured in "charge" units (bytes for the block cache). The
// cache is split into 2^num_shard_bits independently locked shards to reduce
// contention; each shard owns capacity / num_shards.
//
// Values are shared_ptr: an entry evicted while a reader still uses it stays
// alive until the reader drops its reference.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lsm {

struct CacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t inserts = 0;
  uint64_t evictions = 0;
  size_t usage = 0;
  size_t entries = 0;
};

class LRUCache {
 public:
  explicit LRUCache(size_t capacity, int num_shard_bits = 4);
  ~LRUCache();
  LRUCache(const LRUCache&) = delete;
  LRUCache& operator=(const LRUCache&) = delete;

  // Inserts or replaces the mapping for key, then evicts least recently
  // used entries until the shard is within capacity. With zero capacity
  // nothing is cached.
  void Insert(std::string_view key, std::shared_ptr<void> value,
              size_t charge);

  // Returns the cached value (marking it most recently used) or nullptr.
  std::shared_ptr<void> Lookup(std::string_view key);

  template <typename T>
  std::shared_ptr<T> LookupAs(std::string_view key) {
    return std::static_pointer_cast<T>(Lookup(key));
  }

  // Returns true if an entry was removed.
  bool Erase(std::string_view key);
  void Clear();

  size_t Capacity() const { return capacity_; }
  size_t TotalCharge() const;
  size_t EntryCount() const;
  CacheStats GetStats() const;

  // A new id for partitioning the key space (e.g. one per SSTable).
  uint64_t NewId() { return next_id_.fetch_add(1, std::memory_order_relaxed); }

 private:
  class Shard;
  Shard& ShardFor(std::string_view key);

  size_t capacity_;
  int shard_bits_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<uint64_t> next_id_{1};
};

}  // namespace lsm
