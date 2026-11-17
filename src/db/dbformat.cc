#include "db/dbformat.h"

#include "util/coding.h"

namespace lsm {

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq,
                            ValueType type) {
  std::string result;
  result.reserve(user_key.size() + kInternalKeyTrailerSize);
  AppendInternalKey(&result, ParsedInternalKey(user_key, seq, type));
  return result;
}

bool ParseInternalKey(std::string_view internal_key,
                      ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < kInternalKeyTrailerSize) {
    return false;
  }
  const uint64_t tag = DecodeFixed64(internal_key.data() + n - 8);
  const auto type = static_cast<uint8_t>(tag & 0xFF);
  if (!IsValidValueType(type)) {
    return false;
  }
  result->user_key = internal_key.substr(0, n - kInternalKeyTrailerSize);
  result->sequence = tag >> 8;
  result->type = static_cast<ValueType>(type);
  return true;
}

int InternalKeyComparator::Compare(std::string_view a,
                                   std::string_view b) const {
  if (a.size() < kInternalKeyTrailerSize ||
      b.size() < kInternalKeyTrailerSize) {
    // Malformed keys (only possible with corrupted input): fall back to a
    // plain byte order so the comparison is still a total order.
    return user_comparator_->Compare(a, b);
  }
  int r = user_comparator_->Compare(ExtractUserKey(a), ExtractUserKey(b));
  if (r == 0) {
    const uint64_t anum = DecodeFixed64(a.data() + a.size() - 8);
    const uint64_t bnum = DecodeFixed64(b.data() + b.size() - 8);
    if (anum > bnum) {
      r = -1;  // newer (larger sequence) sorts first
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  const size_t internal_size = user_key.size() + kInternalKeyTrailerSize;
  data_.reserve(internal_size + 5);
  PutVarint32(&data_, static_cast<uint32_t>(internal_size));
  key_start_ = data_.size();
  AppendInternalKey(&data_,
                    ParsedInternalKey(user_key, sequence, kValueTypeForSeek));
}

}  // namespace lsm
