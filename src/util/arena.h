// Bump-pointer allocator owning the memory of a MemTable. Memory is freed all
// at once when the arena is destroyed; MemoryUsage() drives the flush policy.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace lsm {

class Arena {
 public:
  static constexpr size_t kBlockSize = 4096;

  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena();

  // Returns a pointer to a newly allocated block of `bytes` bytes (> 0).
  char* Allocate(size_t bytes);
  // Same, aligned to alignof(std::max_align_t).
  char* AllocateAligned(size_t bytes);

  // Total memory allocated by the arena, including bookkeeping overhead.
  // Safe to call concurrently with allocations.
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

}  // namespace lsm
