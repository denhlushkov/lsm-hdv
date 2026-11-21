// Writes an immutable SSTable file from keys added in increasing order.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "filter/bloom_filter.h"
#include "lsm/env.h"
#include "lsm/status.h"
#include "sstable/block_builder.h"
#include "sstable/format.h"
#include "sstable/table_options.h"

namespace lsm {

class TableBuilder {
 public:
  // file must outlive the builder; the builder does not Sync/Close it.
  TableBuilder(const TableOptions& options, WritableFile* file);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  ~TableBuilder();

  // Keys must be strictly increasing w.r.t. options.comparator; a violation
  // makes the builder fail with InvalidArgument.
  void Add(std::string_view key, std::string_view value);

  // Writes the remaining data block, the filter and index blocks and the
  // footer. No more Add() calls are allowed afterwards.
  Status Finish();
  // Stops building; the partially written file should be deleted.
  void Abandon();

  Status status() const { return status_; }
  uint64_t NumEntries() const { return num_entries_; }
  uint64_t NumDataBlocks() const { return num_data_blocks_; }
  // Bytes written so far (the final file size after Finish()).
  uint64_t FileSize() const { return offset_; }

 private:
  bool ok() const { return status_.ok(); }
  void FlushDataBlock();
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(std::string_view contents, BlockHandle* handle);

  TableOptions options_;
  WritableFile* file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  uint64_t num_entries_ = 0;
  uint64_t num_data_blocks_ = 0;
  bool closed_ = false;
  std::unique_ptr<BloomFilterPolicy> filter_policy_;
  std::unique_ptr<FilterBlockBuilder> filter_builder_;
};

}  // namespace lsm
