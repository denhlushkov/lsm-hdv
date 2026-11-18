#include "memtable/memtable.h"

#include <cstring>
#include <limits>

#include "util/coding.h"

namespace lsm {

namespace {

// Decodes a varint32 length-prefixed slice from an arena record. Records are
// produced by MemTable::Add, so they are well formed.
std::string_view GetLengthPrefixed(const char* data) {
  uint32_t len = 0;
  const char* p = GetVarint32Ptr(data, data + 5, &len);
  return std::string_view(p, len);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator.Compare(GetLengthPrefixed(a), GetLengthPrefixed(b));
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), table_(comparator_, &arena_) {}

Status MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                     std::string_view value) {
  if (IsImmutable()) {
    return Status::InvalidArgument("memtable is immutable");
  }
  if (!IsValidValueType(type)) {
    return Status::InvalidArgument("unknown value type");
  }
  if (seq > kMaxSequenceNumber) {
    return Status::InvalidArgument("sequence number overflow");
  }
  constexpr size_t kMaxLen = std::numeric_limits<uint32_t>::max() - 8;
  if (key.size() > kMaxLen || value.size() > kMaxLen) {
    return Status::InvalidArgument("key or value too large");
  }

  const size_t internal_key_size = key.size() + kInternalKeyTrailerSize;
  const size_t encoded_len = static_cast<size_t>(VarintLength(internal_key_size)) +
                             internal_key_size +
                             static_cast<size_t>(VarintLength(value.size())) +
                             value.size();
  char* const buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  if (!key.empty()) {
    std::memcpy(p, key.data(), key.size());
  }
  p += key.size();
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += kInternalKeyTrailerSize;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  if (!value.empty()) {
    std::memcpy(p, value.data(), value.size());
  }
  table_.Insert(buf);
  return Status::OK();
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  Table::Iterator iter(&table_);
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) {
    return false;
  }
  // The iterator is at the first entry whose internal key is >= the lookup
  // key, i.e. the newest version with sequence <= the lookup sequence -- if
  // it belongs to the same user key.
  const std::string_view internal_key = GetLengthPrefixed(iter.key());
  const Comparator* ucmp = comparator_.comparator.user_comparator();
  if (ucmp->Compare(ExtractUserKey(internal_key), key.user_key()) != 0) {
    return false;
  }
  const uint64_t tag =
      DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  switch (static_cast<ValueType>(tag & 0xFF)) {
    case kTypeValue: {
      const std::string_view v =
          GetLengthPrefixed(internal_key.data() + internal_key.size());
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case kTypeDeletion:
      *s = Status::NotFound();
      return true;
  }
  return false;
}

std::unique_ptr<MemTableIterator> MemTable::NewIterator() const {
  // weak_from_this() is empty when the table is not owned by a shared_ptr;
  // the caller must then keep the table alive itself.
  std::shared_ptr<const MemTable> owner = weak_from_this().lock();
  return std::unique_ptr<MemTableIterator>(
      new MemTableIterator(std::move(owner), &table_));
}

const char* MemTableIterator::EncodeTarget(std::string_view internal_key) {
  tmp_.clear();
  PutVarint32(&tmp_, static_cast<uint32_t>(internal_key.size()));
  tmp_.append(internal_key.data(), internal_key.size());
  return tmp_.data();
}

void MemTableIterator::Seek(std::string_view internal_key) {
  iter_.Seek(EncodeTarget(internal_key));
}

void MemTableIterator::SeekForPrev(std::string_view internal_key) {
  iter_.SeekForPrev(EncodeTarget(internal_key));
}

std::string_view MemTableIterator::key() const {
  return GetLengthPrefixed(iter_.key());
}

std::string_view MemTableIterator::value() const {
  const std::string_view k = GetLengthPrefixed(iter_.key());
  return GetLengthPrefixed(k.data() + k.size());
}

}  // namespace lsm
