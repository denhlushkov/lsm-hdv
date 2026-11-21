#include "sstable/block.h"

#include <limits>

#include "util/coding.h"

namespace lsm {

Block::Block(std::string contents) : data_(std::move(contents)) {
  constexpr size_t kU32 = sizeof(uint32_t);
  if (data_.size() < kU32 ||
      data_.size() > std::numeric_limits<uint32_t>::max()) {
    return;
  }
  num_restarts_ = DecodeFixed32(data_.data() + data_.size() - kU32);
  const size_t max_restarts = (data_.size() - kU32) / kU32;
  if (num_restarts_ == 0 || num_restarts_ > max_restarts) {
    num_restarts_ = 0;
    return;
  }
  restart_offset_ =
      static_cast<uint32_t>(data_.size() - (1 + num_restarts_) * kU32);
  ok_ = true;
}

namespace {

// Decodes the three varint header fields of an entry starting at p.
// Returns a pointer to the key delta, or nullptr if the entry does not fit
// in [p, limit).
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                        uint32_t* non_shared, uint32_t* value_length) {
  if (limit - p < 3) {
    return nullptr;
  }
  *shared = static_cast<uint8_t>(p[0]);
  *non_shared = static_cast<uint8_t>(p[1]);
  *value_length = static_cast<uint8_t>(p[2]);
  if ((*shared | *non_shared | *value_length) < 128) {
    p += 3;  // fast path: all three values fit in one byte
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }
  const uint64_t needed = uint64_t{*non_shared} + *value_length;
  if (static_cast<uint64_t>(limit - p) < needed) {
    return nullptr;
  }
  return p;
}

}  // namespace

class BlockIterator final : public Iterator {
 public:
  BlockIterator(std::shared_ptr<const Block> block,
                const Comparator* comparator)
      : block_(std::move(block)),
        comparator_(comparator),
        data_(block_->data_.data()),
        restarts_(block_->restart_offset_),
        num_restarts_(block_->num_restarts_),
        current_(restarts_),
        restart_index_(num_restarts_) {}

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }

  void Next() override { ParseNextKey(); }

  void SeekToFirst() override {
    if (!SeekToRestartPoint(0)) {
      return;
    }
    ParseNextKey();
  }

  void Seek(std::string_view target) override {
    // Binary search over the restart points for the last one whose key is
    // < target; then scan linearly for the first key >= target.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = left + (right - left + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      if (region_offset >= restarts_) {
        CorruptionError();
        return;
      }
      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                        &shared, &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      const std::string_view mid_key(key_ptr, non_shared);
      if (comparator_->Compare(mid_key, target) < 0) {
        left = mid;  // all keys before mid are < target
      } else {
        right = mid - 1;  // keys at and after mid are >= target
      }
    }

    if (!SeekToRestartPoint(left)) {
      return;
    }
    while (ParseNextKey()) {
      if (comparator_->Compare(key_, target) >= 0) {
        return;
      }
    }
  }

 private:
  uint32_t GetRestartPoint(uint32_t index) const {
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  bool SeekToRestartPoint(uint32_t index) {
    const uint32_t offset = GetRestartPoint(index);
    if (offset > restarts_) {
      CorruptionError();
      return false;
    }
    key_.clear();
    restart_index_ = index;
    // ParseNextKey() starts right after value_.
    value_ = std::string_view(data_ + offset, 0);
    return true;
  }

  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = {};
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) {
      // No more entries: mark as invalid.
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  std::shared_ptr<const Block> block_;
  const Comparator* const comparator_;
  const char* const data_;
  uint32_t const restarts_;      // offset of the restart array
  uint32_t const num_restarts_;
  uint32_t current_;             // offset of the current entry
  uint32_t restart_index_;       // restart block containing current_
  std::string key_;
  std::string_view value_;
  Status status_;
};

std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block,
                                           const Comparator* comparator) {
  if (!block || !block->ok()) {
    return NewErrorIterator(Status::Corruption("malformed block contents"));
  }
  return std::make_unique<BlockIterator>(std::move(block), comparator);
}

}  // namespace lsm
