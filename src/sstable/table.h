// Read side of an SSTable (design doc, section 3.2, step 3):
//   1. probe the Bloom filter (skip the file on a negative answer);
//   2. binary search the in-memory index block for the data block;
//   3. fetch the block from the LRU block cache or from disk;
//   4. search inside the data block.
//
// A Table is immutable and safe for concurrent use. Tables are always owned
// by a shared_ptr; iterators share that ownership, so a table (and its file)
// outlives any iterator reading it (design doc, section 5.2).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "cache/lru_cache.h"
#include "lsm/env.h"
#include "lsm/iterator.h"
#include "lsm/status.h"
#include "sstable/table_options.h"

namespace lsm {

class Block;

struct TableReadStats {
  uint64_t block_reads = 0;       // data blocks read from the file
  uint64_t cache_hits = 0;        // data blocks served by the block cache
  uint64_t filter_negatives = 0;  // point lookups rejected by the filter
};

class Table : public std::enable_shared_from_this<Table> {
 public:
  // Opens the table stored in file (file_size bytes long). block_cache may
  // be nullptr. Returns Corruption for anything that is not a valid table.
  static Status Open(const TableOptions& options,
                     std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                     LRUCache* block_cache, std::shared_ptr<Table>* table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  ~Table();

  // Iterator over all entries, in key order.
  std::unique_ptr<Iterator> NewIterator() const;

  // Finds the first entry with key >= `key` in the data block that may
  // contain `key` and passes it to handler. The handler is not called if
  // the filter excludes the key or no such entry exists.
  Status InternalGet(
      std::string_view key,
      const std::function<void(std::string_view, std::string_view)>& handler)
      const;

  // Exact-match lookup. NotFound if the key is absent.
  Status Get(std::string_view key, std::string* value) const;

  // False if the Bloom filter proves that no entry has this key.
  bool KeyMayMatch(std::string_view key) const;
  bool has_filter() const { return !filter_.empty(); }

  uint64_t file_size() const { return file_size_; }
  TableReadStats GetStats() const;

 private:
  Table(const TableOptions& options, std::unique_ptr<RandomAccessFile> file,
        uint64_t file_size, LRUCache* cache,
        std::shared_ptr<const Block> index_block, std::string filter);

  friend class TableIterator;

  // Reads (through the cache) the data block whose encoded handle is given.
  Status ReadDataBlock(std::string_view encoded_handle,
                       std::shared_ptr<const Block>* block) const;
  std::unique_ptr<Iterator> NewDataBlockIterator(
      std::string_view encoded_handle) const;
  std::string_view FilterKey(std::string_view key) const;

  const TableOptions options_;
  const std::unique_ptr<RandomAccessFile> file_;
  const uint64_t file_size_;
  LRUCache* const cache_;
  const uint64_t cache_id_;
  const std::shared_ptr<const Block> index_block_;
  const std::string filter_;

  mutable std::atomic<uint64_t> block_reads_{0};
  mutable std::atomic<uint64_t> cache_hits_{0};
  mutable std::atomic<uint64_t> filter_negatives_{0};
};

}  // namespace lsm
