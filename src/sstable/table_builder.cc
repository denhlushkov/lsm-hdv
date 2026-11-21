#include "sstable/table_builder.h"

#include <cassert>

#include "db/dbformat.h"
#include "util/coding.h"
#include "util/crc32.h"

namespace lsm {

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval, options.comparator),
      // Index entries are looked up by binary search over every entry.
      index_block_(1, options.comparator) {
  if (options_.bloom_bits_per_key > 0) {
    filter_policy_ =
        std::make_unique<BloomFilterPolicy>(options_.bloom_bits_per_key);
    filter_builder_ = std::make_unique<FilterBlockBuilder>(filter_policy_.get());
  }
}

TableBuilder::~TableBuilder() {
  // Either Finish() or Abandon() must have been called.
  assert(closed_);
}

void TableBuilder::Add(std::string_view key, std::string_view value) {
  assert(!closed_);
  if (!ok()) {
    return;
  }
  if (num_entries_ > 0 && options_.comparator->Compare(key, last_key_) <= 0) {
    status_ = Status::InvalidArgument(
        "sstable keys must be added in strictly increasing order");
    return;
  }
  if (filter_builder_) {
    filter_builder_->AddKey(options_.filter_on_user_key ? ExtractUserKey(key)
                                                        : key);
  }
  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  data_block_.Add(key, value);
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  if (!ok() || data_block_.empty()) {
    return;
  }
  BlockHandle handle;
  WriteBlock(&data_block_, &handle);
  if (!ok()) {
    return;
  }
  // The index maps the largest key of each data block to its location.
  std::string encoded_handle;
  handle.EncodeTo(&encoded_handle);
  index_block_.Add(last_key_, encoded_handle);
  ++num_data_blocks_;
  status_ = file_->Flush();
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  WriteRawBlock(block->Finish(), handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(std::string_view contents,
                                 BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(contents.size());
  status_ = file_->Append(contents);
  if (!ok()) {
    return;
  }
  char trailer[kBlockTrailerSize];
  EncodeFixed32(trailer, crc32::Value(contents));
  status_ = file_->Append(std::string_view(trailer, sizeof(trailer)));
  if (ok()) {
    offset_ += contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::Finish() {
  assert(!closed_);
  FlushDataBlock();
  closed_ = true;

  BlockHandle filter_handle(0, 0);
  if (ok() && filter_builder_) {
    const std::string filter = filter_builder_->Finish();
    WriteRawBlock(filter, &filter_handle);
  }

  BlockHandle index_handle;
  if (ok()) {
    WriteBlock(&index_block_, &index_handle);
  }

  if (ok()) {
    Footer footer;
    footer.set_filter_handle(filter_handle);
    footer.set_index_handle(index_handle);
    std::string encoded;
    footer.EncodeTo(&encoded);
    status_ = file_->Append(encoded);
    if (ok()) {
      offset_ += encoded.size();
      status_ = file_->Flush();
    }
  }
  return status_;
}

void TableBuilder::Abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace lsm
