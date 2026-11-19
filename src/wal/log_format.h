// Write-Ahead Log record format (design doc, section 4.1):
//
//   +---------+------+-----------+-----------+-----------+-------------+
//   | CRC32   | Type | Key len   | Value len | Key bytes | Value bytes |
//   | 4 bytes | 1 B  | 4 B (LE)  | 4 B (LE)  | key len   | value len   |
//   +---------+------+-----------+-----------+-----------+-------------+
//
// The CRC32 (IEEE 802.3) covers every byte after the checksum field.
// Type is 0x01 for PUT and 0x02 for DELETE (value length must be 0).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lsm::wal {

enum class RecordType : uint8_t {
  kPut = 0x01,
  kDelete = 0x02,
};

inline constexpr size_t kChecksumSize = 4;
inline constexpr size_t kHeaderSize = kChecksumSize + 1 + 4 + 4;  // 13 bytes

// Upper bound on key + value size accepted by the reader. Protects against
// huge allocations when a length field is corrupted.
inline constexpr size_t kDefaultMaxRecordSize = size_t{64} << 20;

struct Record {
  RecordType type = RecordType::kPut;
  std::string key;
  std::string value;
};

// Appends the binary encoding of one record to *dst.
void EncodeRecord(RecordType type, std::string_view key, std::string_view value,
                  std::string* dst);

}  // namespace lsm::wal
