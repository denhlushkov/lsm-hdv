// MemTable: in-memory sorted buffer of recent writes (design doc, 2.1.2).
//
// Each entry is stored in the arena as
//   varint32(internal_key_len) | internal_key | varint32(value_len) | value
// and indexed by a SkipList of pointers to these records. Deletes are stored
// as tombstone entries (kTypeDeletion) rather than removing anything.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/status.h"
#include "memtable/skiplist.h"
#include "util/arena.h"

namespace lsm {

class MemTableIterator;

class MemTable : public std::enable_shared_from_this<MemTable> {
 public:
  explicit MemTable(const InternalKeyComparator& comparator);
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Adds an entry. Requires external synchronization between writers;
  // readers may run concurrently. Fails once the table is immutable.
  Status Add(SequenceNumber seq, ValueType type, std::string_view key,
             std::string_view value);
  Status Put(SequenceNumber seq, std::string_view key, std::string_view value) {
    return Add(seq, kTypeValue, key, value);
  }
  Status Delete(SequenceNumber seq, std::string_view key) {
    return Add(seq, kTypeDeletion, key, {});
  }

  // Looks up the newest entry for key.user_key() with sequence <= the lookup
  // sequence. Returns true if such an entry exists; then *s is OK and *value
  // holds the value, or *s is NotFound for a tombstone. Returns false if the
  // MemTable knows nothing about the key (older data must be consulted).
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

  // Iterator over internal keys. If the MemTable is owned by a shared_ptr,
  // the iterator keeps it alive (design doc, section 5.2).
  std::unique_ptr<MemTableIterator> NewIterator() const;

  // Bytes of memory used, compared against Options::write_buffer_size.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  bool ShouldFlush(size_t write_buffer_size) const {
    return ApproximateMemoryUsage() >= write_buffer_size;
  }

  size_t NumEntries() const { return table_.Size(); }
  bool Empty() const { return NumEntries() == 0; }

  // Freezes the table before it is flushed to an SSTable (design doc, 3.1).
  void MarkImmutable() { immutable_.store(true, std::memory_order_release); }
  bool IsImmutable() const {
    return immutable_.load(std::memory_order_acquire);
  }

  const InternalKeyComparator& internal_comparator() const {
    return comparator_.comparator;
  }

 private:
  friend class MemTableIterator;

  struct KeyComparator {
    InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;

  KeyComparator comparator_;
  Arena arena_;
  Table table_;
  std::atomic<bool> immutable_{false};
};

// Bidirectional iterator over a MemTable; key() is an internal key.
class MemTableIterator final : public Iterator {
 public:
  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() { iter_.SeekToLast(); }
  void Seek(std::string_view internal_key) override;
  // Position at the last entry with key <= internal_key.
  void SeekForPrev(std::string_view internal_key);
  void Next() override { iter_.Next(); }
  void Prev() { iter_.Prev(); }
  std::string_view key() const override;
  std::string_view value() const override;
  Status status() const override { return Status::OK(); }

 private:
  friend class MemTable;
  MemTableIterator(std::shared_ptr<const MemTable> owner,
                   const MemTable::Table* table)
      : owner_(std::move(owner)), iter_(table) {}

  const char* EncodeTarget(std::string_view internal_key);

  std::shared_ptr<const MemTable> owner_;
  MemTable::Table::Iterator iter_;
  std::string tmp_;  // scratch space for encoding seek targets
};

}  // namespace lsm
