#include "wal/log_reader.h"

#include <algorithm>

#include "util/coding.h"
#include "util/crc32.h"

namespace lsm::wal {

namespace {
constexpr size_t kReadChunk = 32 * 1024;
}  // namespace

Reader::Reader(SequentialFile* file, size_t max_record_size)
    : file_(file),
      max_record_size_(max_record_size),
      scratch_(std::make_unique<char[]>(kReadChunk)) {}

Status Reader::ReadUpTo(size_t n, std::string* out) {
  out->clear();
  // Read in bounded chunks so that a corrupted length that slipped past the
  // sanity check never causes one huge allocation before EOF is noticed.
  while (out->size() < n) {
    const size_t want = std::min(kReadChunk, n - out->size());
    std::string_view fragment;
    Status s = file_->Read(want, &fragment, scratch_.get());
    if (!s.ok()) {
      return s;
    }
    if (fragment.empty()) {
      break;  // end of file
    }
    out->append(fragment.data(), fragment.size());
  }
  return Status::OK();
}

void Reader::ReportCorruption(const char* reason) {
  status_ = Status::Corruption(std::string(reason) + " at WAL offset " +
                               std::to_string(valid_bytes_));
}

bool Reader::ReadRecord(Record* record) {
  if (eof_ || !status_.ok()) {
    return false;
  }

  Status s = ReadUpTo(kHeaderSize, &header_);
  if (!s.ok()) {
    status_ = s;
    return false;
  }
  if (header_.empty()) {
    eof_ = true;  // clean end of log
    return false;
  }
  if (header_.size() < kHeaderSize) {
    eof_ = true;  // crash while the header was being written
    tail_truncated_ = true;
    return false;
  }

  const uint32_t expected_crc = DecodeFixed32(header_.data());
  const auto type = static_cast<uint8_t>(header_[kChecksumSize]);
  const uint32_t key_len = DecodeFixed32(header_.data() + kChecksumSize + 1);
  const uint32_t value_len = DecodeFixed32(header_.data() + kChecksumSize + 5);

  if (type != static_cast<uint8_t>(RecordType::kPut) &&
      type != static_cast<uint8_t>(RecordType::kDelete)) {
    ReportCorruption("unknown record type");
    return false;
  }
  if (type == static_cast<uint8_t>(RecordType::kDelete) && value_len != 0) {
    ReportCorruption("DELETE record with a value");
    return false;
  }
  const uint64_t payload_len = uint64_t{key_len} + value_len;
  if (payload_len > max_record_size_) {
    ReportCorruption("record length exceeds limit");
    return false;
  }

  s = ReadUpTo(static_cast<size_t>(payload_len), &payload_);
  if (!s.ok()) {
    status_ = s;
    return false;
  }
  if (payload_.size() < payload_len) {
    eof_ = true;  // crash while the payload was being written
    tail_truncated_ = true;
    return false;
  }

  uint32_t actual_crc = crc32::Value(header_.data() + kChecksumSize,
                                     kHeaderSize - kChecksumSize);
  actual_crc = crc32::Extend(actual_crc, payload_.data(), payload_.size());
  if (actual_crc != expected_crc) {
    ReportCorruption("checksum mismatch");
    return false;
  }

  record->type = static_cast<RecordType>(type);
  record->key.assign(payload_.data(), key_len);
  record->value.assign(payload_.data() + key_len, value_len);
  valid_bytes_ += kHeaderSize + payload_len;
  ++records_read_;
  return true;
}

}  // namespace lsm::wal
