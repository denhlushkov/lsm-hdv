// Iterator interface (design doc, section 5).
#pragma once

#include <memory>
#include <string_view>

#include "lsm/status.h"

namespace lsm {

class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator() = default;

  // An iterator is either positioned at an entry or is not valid.
  virtual bool Valid() const = 0;
  // Position at the first entry of the source.
  virtual void SeekToFirst() = 0;
  // Position at the first entry with key >= target.
  virtual void Seek(std::string_view target) = 0;
  // Advance to the next entry. REQUIRES: Valid().
  virtual void Next() = 0;
  // The returned views stay valid until the iterator is moved or destroyed.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  // Non-OK if an error (e.g. on-disk corruption) has been encountered.
  virtual Status status() const = 0;
};

// An iterator over nothing; status() is OK.
std::unique_ptr<Iterator> NewEmptyIterator();
// An iterator over nothing that reports the given error status.
std::unique_ptr<Iterator> NewErrorIterator(const Status& status);

}  // namespace lsm
