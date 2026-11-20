#include "cache/lru_cache.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#include "util/hash.h"

namespace lsm {

namespace {

struct LRUNode {
  std::string key;
  std::shared_ptr<void> value;
  size_t charge = 0;
  LRUNode* prev = nullptr;
  LRUNode* next = nullptr;
};

}  // namespace

class LRUCache::Shard {
 public:
  explicit Shard(size_t capacity) : capacity_(capacity) {
    // Circular list with a sentinel: head_.next is the least recently used
    // entry, head_.prev the most recently used one.
    head_.next = &head_;
    head_.prev = &head_;
  }

  ~Shard() { Clear(); }

  void Insert(std::string_view key, std::shared_ptr<void> value,
              size_t charge) {
    std::lock_guard<std::mutex> lock(mu_);
    if (capacity_ == 0) {
      return;
    }
    auto it = table_.find(key);
    if (it != table_.end()) {
      RemoveNode(it->second);
    }
    auto* node = new LRUNode;
    node->key.assign(key.data(), key.size());
    node->value = std::move(value);
    node->charge = charge;
    table_.emplace(std::string_view(node->key), node);
    AppendMostRecent(node);
    usage_ += charge;
    ++inserts_;
    // Evict from the cold end. If the new entry alone exceeds the capacity
    // it is evicted as well, leaving the cache within its limit.
    while (usage_ > capacity_ && head_.next != &head_) {
      RemoveNode(head_.next);
      ++evictions_;
    }
  }

  std::shared_ptr<void> Lookup(std::string_view key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key);
    if (it == table_.end()) {
      ++misses_;
      return nullptr;
    }
    ++hits_;
    LRUNode* node = it->second;
    Unlink(node);
    AppendMostRecent(node);
    return node->value;
  }

  bool Erase(std::string_view key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_.find(key);
    if (it == table_.end()) {
      return false;
    }
    RemoveNode(it->second);
    return true;
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    while (head_.next != &head_) {
      RemoveNode(head_.next);
    }
  }

  void AccumulateStats(CacheStats* stats) const {
    std::lock_guard<std::mutex> lock(mu_);
    stats->hits += hits_;
    stats->misses += misses_;
    stats->inserts += inserts_;
    stats->evictions += evictions_;
    stats->usage += usage_;
    stats->entries += table_.size();
  }

 private:
  void Unlink(LRUNode* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = nullptr;
  }

  void AppendMostRecent(LRUNode* node) {
    node->next = &head_;
    node->prev = head_.prev;
    node->prev->next = node;
    node->next->prev = node;
  }

  // Requires mu_ held. Unlinks, uncounts and frees the node.
  void RemoveNode(LRUNode* node) {
    table_.erase(std::string_view(node->key));  // before the key is freed
    Unlink(node);
    usage_ -= node->charge;
    delete node;
  }

  const size_t capacity_;
  mutable std::mutex mu_;
  LRUNode head_;
  std::unordered_map<std::string_view, LRUNode*> table_;
  size_t usage_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t inserts_ = 0;
  uint64_t evictions_ = 0;
};

LRUCache::LRUCache(size_t capacity, int num_shard_bits)
    : capacity_(capacity), shard_bits_(std::clamp(num_shard_bits, 0, 8)) {
  const size_t num_shards = size_t{1} << shard_bits_;
  const size_t per_shard = (capacity + num_shards - 1) / num_shards;
  shards_.reserve(num_shards);
  for (size_t i = 0; i < num_shards; ++i) {
    shards_.push_back(std::make_unique<Shard>(per_shard));
  }
}

LRUCache::~LRUCache() = default;

LRUCache::Shard& LRUCache::ShardFor(std::string_view key) {
  if (shard_bits_ == 0) {
    return *shards_[0];
  }
  const uint32_t h = Hash32(key);
  return *shards_[h >> (32 - shard_bits_)];
}

void LRUCache::Insert(std::string_view key, std::shared_ptr<void> value,
                      size_t charge) {
  ShardFor(key).Insert(key, std::move(value), charge);
}

std::shared_ptr<void> LRUCache::Lookup(std::string_view key) {
  return ShardFor(key).Lookup(key);
}

bool LRUCache::Erase(std::string_view key) { return ShardFor(key).Erase(key); }

void LRUCache::Clear() {
  for (auto& shard : shards_) {
    shard->Clear();
  }
}

CacheStats LRUCache::GetStats() const {
  CacheStats stats;
  for (const auto& shard : shards_) {
    shard->AccumulateStats(&stats);
  }
  return stats;
}

size_t LRUCache::TotalCharge() const { return GetStats().usage; }

size_t LRUCache::EntryCount() const { return GetStats().entries; }

}  // namespace lsm
