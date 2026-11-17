// Internal key format shared by the MemTable and SSTables.
//
//   internal_key := user_key | fixed64(sequence << 8 | type)
//
// Internal keys are ordered by user key ascending, then by sequence number
// descending, so the newest version of a key is met first during a scan.
#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/comparator.h"

namespace lsm {

using SequenceNumber = uint64_t;

// The low 8 bits of the trailer hold the type, so sequence numbers are 56-bit.
inline constexpr SequenceNumber kMaxSequenceNumber =
    (uint64_t{1} << 56) - 1;

// Values match the WAL record type byte (design doc, section 4.1).
enum ValueType : uint8_t {
  kTypeValue = 0x01,
  kTypeDeletion = 0x02,  // tombstone
};

// With sequence numbers ordered descending and ties broken by the packed
// trailer, the seek target must use the largest type value.
inline constexpr ValueType kValueTypeForSeek = kTypeDeletion;

inline constexpr size_t kInternalKeyTrailerSize = 8;

inline bool IsValidValueType(uint8_t t) {
  return t == kTypeValue || t == kTypeDeletion;
}

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  return (seq << 8) | static_cast<uint64_t>(t);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;

  ParsedInternalKey() = default;
  ParsedInternalKey(std::string_view u, SequenceNumber seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
};

void AppendInternalKey(std::string* result, const ParsedInternalKey& key);
std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq,
                            ValueType type);

// Returns false if the input is too short or has an unknown type.
bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result);

// Returns the user key part. Tolerates malformed (too short) keys by
// returning an empty view, so corrupted input never reads out of bounds.
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  if (internal_key.size() < kInternalKeyTrailerSize) {
    return {};
  }
  return internal_key.substr(0, internal_key.size() - kInternalKeyTrailerSize);
}

class InternalKeyComparator final : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* user_comparator)
      : user_comparator_(user_comparator) {}

  int Compare(std::string_view a, std::string_view b) const override;
  const char* Name() const override { return "lsm.InternalKeyComparator"; }

  const Comparator* user_comparator() const { return user_comparator_; }

 private:
  const Comparator* user_comparator_;
};

// A key used for point lookups at a given sequence number. Holds the
// MemTable form (length prefixed) of the internal key.
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);

  // varint32(internal key length) | internal key
  std::string_view memtable_key() const { return data_; }
  std::string_view internal_key() const {
    return std::string_view(data_).substr(key_start_);
  }
  std::string_view user_key() const {
    return std::string_view(data_).substr(
        key_start_, data_.size() - key_start_ - kInternalKeyTrailerSize);
  }

 private:
  std::string data_;
  size_t key_start_;
};

}  // namespace lsm
