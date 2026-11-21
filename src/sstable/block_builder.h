// Builds a block of sorted key/value pairs with prefix (delta) compression.
//
// Entry encoding:
//   varint32 shared | varint32 non_shared | varint32 value_len |
//   key_delta[non_shared] | value[value_len]
// where `shared` is the length of the prefix common with the previous key.
// Every `restart_interval` entries a restart point stores the full key
// (shared = 0); the block ends with the restart offsets so a reader can
// binary search them:
//   entries... | fixed32 restart[0] ... restart[R-1] | fixed32 R
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/comparator.h"

namespace lsm {

class BlockBuilder {
 public:
  // comparator (optional) is used to check key order in debug builds.
  explicit BlockBuilder(int restart_interval,
                        const Comparator* comparator = nullptr);
  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  void Reset();

  // REQUIRES: Finish() not called since the last Reset();
  //           key is larger than any previously added key.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array. The view stays valid until Reset().
  std::string_view Finish();

  // Size of the block if Finish() were called now.
  size_t CurrentSizeEstimate() const;
  bool empty() const { return buffer_.empty(); }
  size_t num_entries() const { return num_entries_; }

 private:
  const int restart_interval_;
  const Comparator* comparator_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;  // entries since the last restart point
  size_t num_entries_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace lsm
