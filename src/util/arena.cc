#include "util/arena.h"

#include <cassert>
#include <cstdint>

namespace lsm {

namespace {
constexpr size_t kAlign = alignof(std::max_align_t);
static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of 2");
}  // namespace

Arena::~Arena() {
  for (char* block : blocks_) {
    delete[] block;
  }
}

char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

char* Arena::AllocateAligned(size_t bytes) {
  const size_t current_mod =
      reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  const size_t slop = current_mod == 0 ? 0 : kAlign - current_mod;
  const size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returns memory aligned by operator new[].
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
  return result;
}

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Large object: give it its own block so the current block's remaining
    // space is not wasted.
    return AllocateNewBlock(bytes);
  }
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}

}  // namespace lsm
