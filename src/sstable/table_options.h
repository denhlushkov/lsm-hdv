// Options for building and reading a single SSTable.
#pragma once

#include <cstddef>

#include "lsm/comparator.h"
#include "lsm/options.h"

namespace lsm {

struct TableOptions {
  // Order of the keys stored in the table.
  const Comparator* comparator = BytewiseComparator();
  size_t block_size = 4 * 1024;
  int block_restart_interval = 16;
  // <= 0 disables the Bloom filter block.
  int bloom_bits_per_key = 10;
  // When the table stores internal keys (user key + 8-byte trailer) the
  // filter is built over, and probed with, the user key part.
  bool filter_on_user_key = false;
  bool verify_checksums = true;

  static TableOptions FromOptions(const Options& options,
                                  const Comparator* key_comparator,
                                  bool internal_keys) {
    TableOptions t;
    t.comparator = key_comparator;
    t.block_size = options.block_size;
    t.block_restart_interval = options.block_restart_interval;
    t.bloom_bits_per_key = options.bloom_bits_per_key;
    t.filter_on_user_key = internal_keys;
    t.verify_checksums = options.verify_checksums;
    return t;
  }
};

}  // namespace lsm
