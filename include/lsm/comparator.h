// Total order over keys.
#pragma once

#include <string_view>

namespace lsm {

class Comparator {
 public:
  virtual ~Comparator() = default;
  // <0 if a < b, 0 if a == b, >0 if a > b.
  virtual int Compare(std::string_view a, std::string_view b) const = 0;
  // Name of the ordering; persisted formats are only valid under one name.
  virtual const char* Name() const = 0;
};

// Lexicographic order over unsigned bytes (memcmp order). Never deleted.
const Comparator* BytewiseComparator();

}  // namespace lsm
