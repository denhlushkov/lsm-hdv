#include "sstable/format.h"

#include <memory>

#include "util/coding.h"
#include "util/crc32.h"

namespace lsm {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  PutFixed64(dst, filter_handle_.offset());
  PutFixed64(dst, filter_handle_.size());
  PutFixed64(dst, index_handle_.offset());
  PutFixed64(dst, index_handle_.size());
  PutFixed32(dst, kFormatVersion);
  PutFixed32(dst, crc32::Value(dst->data() + start, dst->size() - start));
  PutFixed64(dst, kTableMagicNumber);
}

Status Footer::DecodeFrom(std::string_view input) {
  if (input.size() != kEncodedLength) {
    return Status::Corruption("footer has wrong length");
  }
  const char* p = input.data();
  if (DecodeFixed64(p + 40) != kTableMagicNumber) {
    return Status::Corruption("bad magic number: not an sstable");
  }
  if (DecodeFixed32(p + 36) != crc32::Value(p, 36)) {
    return Status::Corruption("footer checksum mismatch");
  }
  if (DecodeFixed32(p + 32) != kFormatVersion) {
    return Status::Corruption("unsupported sstable format version");
  }
  filter_handle_ = BlockHandle(DecodeFixed64(p), DecodeFixed64(p + 8));
  index_handle_ = BlockHandle(DecodeFixed64(p + 16), DecodeFixed64(p + 24));
  return Status::OK();
}

Status ReadBlock(const RandomAccessFile* file, uint64_t data_limit,
                 const BlockHandle& handle, bool verify_checksum,
                 std::string* contents) {
  contents->clear();
  // Overflow-safe check of offset + size + trailer <= data_limit.
  if (handle.offset() > data_limit || handle.size() > data_limit ||
      data_limit - handle.offset() < handle.size() ||
      data_limit - handle.offset() - handle.size() < kBlockTrailerSize) {
    return Status::Corruption("block handle out of range");
  }
  const size_t n = static_cast<size_t>(handle.size());
  const size_t total = n + kBlockTrailerSize;
  auto buf = std::make_unique<char[]>(total);
  std::string_view result;
  Status s = file->Read(handle.offset(), total, &result, buf.get());
  if (!s.ok()) {
    return s;
  }
  if (result.size() != total) {
    return Status::Corruption("truncated block read");
  }
  if (verify_checksum) {
    const uint32_t expected = DecodeFixed32(result.data() + n);
    const uint32_t actual = crc32::Value(result.data(), n);
    if (expected != actual) {
      return Status::Corruption("block checksum mismatch");
    }
  }
  contents->assign(result.data(), n);
  return Status::OK();
}

}  // namespace lsm
