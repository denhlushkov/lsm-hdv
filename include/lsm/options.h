// Engine options (design doc, section 5).
#pragma once

#include <cstddef>

#include "lsm/comparator.h"

namespace lsm {

class Env;

struct Options {
  // MemTable size after which it becomes immutable and is flushed (4 MB).
  size_t write_buffer_size = 4 * 1024 * 1024;
  // Target size of an uncompressed SSTable data block (4 KB).
  size_t block_size = 4 * 1024;
  // Capacity of the LRU block cache in bytes (32 MB).
  size_t block_cache_capacity = 32 * 1024 * 1024;
  // Bloom filter bits per key; 10 bits gives ~1% false positives.
  // A value <= 0 disables filters.
  int bloom_bits_per_key = 10;
  // fsync the WAL after every Put/Delete.
  bool sync_wal = false;

  // --- Extensions over the design document --------------------------------
  // Number of keys between restart points for prefix (delta) compression.
  int block_restart_interval = 16;
  // Verify CRC32 of every block read from an SSTable.
  bool verify_checksums = true;
  // Ordering of user keys.
  const Comparator* comparator = BytewiseComparator();
  // File system; nullptr means Env::Default().
  Env* env = nullptr;
};

}  // namespace lsm
