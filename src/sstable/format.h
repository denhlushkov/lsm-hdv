// SSTable file layout (design doc, section 4.3):
//
//   [data block 0][trailer] ... [data block N][trailer]
//   [filter block][trailer]            (absent if filters are disabled)
//   [index block][trailer]
//   [footer: 48 bytes]
//
// Every block is followed by a 4-byte trailer holding the CRC32 of the block
// contents, so corruption is detected before a block is parsed.
//
// Footer (fixed 48 bytes, little-endian):
//   filter handle  offset u64 | size u64     (size 0 = no filter)
//   index handle   offset u64 | size u64
//   format version u32                        (= 1)
//   footer CRC32   u32  (over the preceding 36 bytes)
//   magic number   u64  0xDB47534D5F4B5631
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/env.h"
#include "lsm/status.h"

namespace lsm {

inline constexpr uint64_t kTableMagicNumber = 0xDB47534D5F4B5631ULL;
inline constexpr size_t kBlockTrailerSize = 4;

// Location of a block inside the file. Index entries store it as two
// varint64 values.
class BlockHandle {
 public:
  static constexpr size_t kMaxEncodedLength = 10 + 10;

  BlockHandle() = default;
  BlockHandle(uint64_t offset, uint64_t size) : offset_(offset), size_(size) {}

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }
  void set_offset(uint64_t offset) { offset_ = offset; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view* input);

 private:
  uint64_t offset_ = 0;
  uint64_t size_ = 0;
};

class Footer {
 public:
  static constexpr size_t kEncodedLength = 48;
  static constexpr uint32_t kFormatVersion = 1;

  const BlockHandle& filter_handle() const { return filter_handle_; }
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_filter_handle(const BlockHandle& h) { filter_handle_ = h; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  // input must be exactly kEncodedLength bytes.
  Status DecodeFrom(std::string_view input);

 private:
  BlockHandle filter_handle_;
  BlockHandle index_handle_;
};

// Reads the block identified by handle and verifies its trailer CRC.
// data_limit is the end of the block area (file size minus footer); handles
// pointing outside of it are rejected before any allocation happens.
Status ReadBlock(const RandomAccessFile* file, uint64_t data_limit,
                 const BlockHandle& handle, bool verify_checksum,
                 std::string* contents);

}  // namespace lsm
