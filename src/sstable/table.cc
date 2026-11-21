#include "sstable/table.h"

#include "db/dbformat.h"
#include "filter/bloom_filter.h"
#include "sstable/block.h"
#include "sstable/format.h"
#include "util/coding.h"

namespace lsm {

Status Table::Open(const TableOptions& options,
                   std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                   LRUCache* block_cache, std::shared_ptr<Table>* table) {
  table->reset();
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  std::string_view footer_input;
  Status s = file->Read(file_size - Footer::kEncodedLength,
                        Footer::kEncodedLength, &footer_input, footer_space);
  if (!s.ok()) {
    return s;
  }
  Footer footer;
  s = footer.DecodeFrom(footer_input);
  if (!s.ok()) {
    return s;
  }

  // Meta blocks are always verified: they are read once per table.
  const uint64_t data_limit = file_size - Footer::kEncodedLength;
  std::string index_contents;
  s = ReadBlock(file.get(), data_limit, footer.index_handle(),
                /*verify_checksum=*/true, &index_contents);
  if (!s.ok()) {
    return s;
  }
  auto index_block = std::make_shared<Block>(std::move(index_contents));
  if (!index_block->ok()) {
    return Status::Corruption("malformed index block");
  }

  std::string filter;
  if (footer.filter_handle().size() > 0) {
    s = ReadBlock(file.get(), data_limit, footer.filter_handle(),
                  /*verify_checksum=*/true, &filter);
    if (!s.ok()) {
      return s;
    }
  }

  table->reset(new Table(options, std::move(file), file_size, block_cache,
                         std::move(index_block), std::move(filter)));
  return Status::OK();
}

Table::Table(const TableOptions& options,
             std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
             LRUCache* cache, std::shared_ptr<const Block> index_block,
             std::string filter)
    : options_(options),
      file_(std::move(file)),
      file_size_(file_size),
      cache_(cache),
      cache_id_(cache != nullptr ? cache->NewId() : 0),
      index_block_(std::move(index_block)),
      filter_(std::move(filter)) {}

Table::~Table() = default;

std::string_view Table::FilterKey(std::string_view key) const {
  return options_.filter_on_user_key ? ExtractUserKey(key) : key;
}

bool Table::KeyMayMatch(std::string_view key) const {
  if (filter_.empty()) {
    return true;
  }
  return BloomFilterPolicy::KeyMayMatch(FilterKey(key), filter_);
}

Status Table::ReadDataBlock(std::string_view encoded_handle,
                            std::shared_ptr<const Block>* block) const {
  BlockHandle handle;
  Status s = handle.DecodeFrom(&encoded_handle);
  if (!s.ok()) {
    return s;
  }

  // Cache key: table id + block offset (16 bytes).
  char cache_key[16];
  if (cache_ != nullptr) {
    EncodeFixed64(cache_key, cache_id_);
    EncodeFixed64(cache_key + 8, handle.offset());
    auto cached = cache_->LookupAs<Block>(std::string_view(cache_key, 16));
    if (cached) {
      cache_hits_.fetch_add(1, std::memory_order_relaxed);
      *block = std::move(cached);
      return Status::OK();
    }
  }

  std::string contents;
  s = ReadBlock(file_.get(), file_size_ - Footer::kEncodedLength, handle,
                options_.verify_checksums, &contents);
  if (!s.ok()) {
    return s;
  }
  block_reads_.fetch_add(1, std::memory_order_relaxed);
  auto new_block = std::make_shared<Block>(std::move(contents));
  if (!new_block->ok()) {
    return Status::Corruption("malformed data block");
  }
  if (cache_ != nullptr) {
    cache_->Insert(std::string_view(cache_key, 16), new_block,
                   new_block->size());
  }
  *block = std::move(new_block);
  return Status::OK();
}

std::unique_ptr<Iterator> Table::NewDataBlockIterator(
    std::string_view encoded_handle) const {
  std::shared_ptr<const Block> block;
  Status s = ReadDataBlock(encoded_handle, &block);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }
  return NewBlockIterator(std::move(block), options_.comparator);
}

Status Table::InternalGet(
    std::string_view key,
    const std::function<void(std::string_view, std::string_view)>& handler)
    const {
  if (!KeyMayMatch(key)) {
    filter_negatives_.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }
  auto index_iter = NewBlockIterator(index_block_, options_.comparator);
  index_iter->Seek(key);
  if (!index_iter->Valid()) {
    return index_iter->status();  // key is past the last block
  }
  auto block_iter = NewDataBlockIterator(index_iter->value());
  block_iter->Seek(key);
  if (block_iter->Valid()) {
    handler(block_iter->key(), block_iter->value());
  }
  return block_iter->status();
}

Status Table::Get(std::string_view key, std::string* value) const {
  bool found = false;
  Status s = InternalGet(key, [&](std::string_view k, std::string_view v) {
    if (options_.comparator->Compare(k, key) == 0) {
      value->assign(v.data(), v.size());
      found = true;
    }
  });
  if (!s.ok()) {
    return s;
  }
  return found ? Status::OK() : Status::NotFound();
}

TableReadStats Table::GetStats() const {
  TableReadStats stats;
  stats.block_reads = block_reads_.load(std::memory_order_relaxed);
  stats.cache_hits = cache_hits_.load(std::memory_order_relaxed);
  stats.filter_negatives = filter_negatives_.load(std::memory_order_relaxed);
  return stats;
}

// Two-level iterator: walks the index block and, for each index entry, the
// data block it points to.
class TableIterator final : public Iterator {
 public:
  explicit TableIterator(std::shared_ptr<const Table> table)
      : table_(std::move(table)),
        index_iter_(NewBlockIterator(table_->index_block_,
                                     table_->options_.comparator)) {}

  bool Valid() const override { return data_iter_ && data_iter_->Valid(); }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_) {
      data_iter_->SeekToFirst();
    }
    SkipEmptyDataBlocksForward();
  }

  void Seek(std::string_view target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_) {
      data_iter_->Seek(target);
    }
    SkipEmptyDataBlocksForward();
  }

  void Next() override {
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  std::string_view key() const override { return data_iter_->key(); }
  std::string_view value() const override { return data_iter_->value(); }

  Status status() const override {
    if (!index_iter_->status().ok()) {
      return index_iter_->status();
    }
    if (data_iter_ && !data_iter_->status().ok()) {
      return data_iter_->status();
    }
    return status_;
  }

 private:
  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      data_iter_.reset();
      return;
    }
    const std::string_view handle = index_iter_->value();
    if (data_iter_ && handle == data_block_handle_) {
      return;  // already positioned in this block
    }
    data_iter_ = table_->NewDataBlockIterator(handle);
    data_block_handle_.assign(handle.data(), handle.size());
  }

  void SkipEmptyDataBlocksForward() {
    while (!data_iter_ || !data_iter_->Valid()) {
      if (data_iter_ && !data_iter_->status().ok()) {
        // Remember the error and stop: a corrupted table is not skipped.
        status_ = data_iter_->status();
        data_iter_.reset();
        return;
      }
      if (!index_iter_->Valid()) {
        data_iter_.reset();
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_) {
        data_iter_->SeekToFirst();
      }
    }
  }

  std::shared_ptr<const Table> table_;
  std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;
  std::string data_block_handle_;
  Status status_;
};

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<TableIterator>(shared_from_this());
}

}  // namespace lsm
