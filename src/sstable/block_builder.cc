#include "sstable/block_builder.h"

#include <algorithm>
#include <cassert>

#include "util/coding.h"

namespace lsm {

BlockBuilder::BlockBuilder(int restart_interval, const Comparator* comparator)
    : restart_interval_(std::max(restart_interval, 1)),
      comparator_(comparator) {
  Reset();
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // the first restart point is at offset 0
  counter_ = 0;
  num_entries_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) +
         sizeof(uint32_t);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  assert(counter_ <= restart_interval_);
  assert(num_entries_ == 0 || comparator_ == nullptr ||
         comparator_->Compare(key, last_key_) > 0);

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) {
      ++shared;
    }
  } else {
    // Start a new restart point: the full key is stored.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(last_key_ == key);
  ++counter_;
  ++num_entries_;
}

std::string_view BlockBuilder::Finish() {
  for (uint32_t restart : restarts_) {
    PutFixed32(&buffer_, restart);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

}  // namespace lsm
