// Read side of a block produced by BlockBuilder. All parsing is bounds
// checked: malformed contents yield Status::Corruption, never out-of-bounds
// reads (design doc, test F-01).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lsm/comparator.h"
#include "lsm/iterator.h"

namespace lsm {

class Block {
 public:
  explicit Block(std::string contents);
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return data_.size(); }
  // False if the restart array is malformed.
  bool ok() const { return ok_; }
  uint32_t num_restarts() const { return num_restarts_; }

 private:
  friend class BlockIterator;

  std::string data_;
  uint32_t restart_offset_ = 0;  // offset of the restart array in data_
  uint32_t num_restarts_ = 0;
  bool ok_ = false;
};

// The iterator shares ownership of the block, so it stays valid even if the
// block is evicted from the cache meanwhile.
std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block,
                                           const Comparator* comparator);

}  // namespace lsm
