#include "wal/log_writer.h"

#include <limits>

#include "util/coding.h"
#include "util/crc32.h"

namespace lsm::wal {

void EncodeRecord(RecordType type, std::string_view key, std::string_view value,
                  std::string* dst) {
  const size_t start = dst->size();
  dst->append(kChecksumSize, '\0');  // placeholder for the CRC
  dst->push_back(static_cast<char>(type));
  PutFixed32(dst, static_cast<uint32_t>(key.size()));
  PutFixed32(dst, static_cast<uint32_t>(value.size()));
  dst->append(key.data(), key.size());
  dst->append(value.data(), value.size());
  const uint32_t crc = crc32::Value(dst->data() + start + kChecksumSize,
                                    dst->size() - start - kChecksumSize);
  EncodeFixed32(dst->data() + start, crc);
}

Writer::Writer(WritableFile* dest, bool sync_each_record)
    : dest_(dest), sync_each_record_(sync_each_record) {}

Status Writer::AddRecord(RecordType type, std::string_view key,
                         std::string_view value) {
  if (!status_.ok()) {
    return status_;
  }
  if (type != RecordType::kPut && type != RecordType::kDelete) {
    return Status::InvalidArgument("unknown WAL record type");
  }
  if (type == RecordType::kDelete && !value.empty()) {
    return Status::InvalidArgument("DELETE record must not carry a value");
  }
  constexpr size_t kMaxLen = std::numeric_limits<uint32_t>::max();
  if (key.size() > kMaxLen || value.size() > kMaxLen) {
    return Status::InvalidArgument("WAL record too large");
  }

  buffer_.clear();
  EncodeRecord(type, key, value, &buffer_);
  // One Append per record keeps torn writes confined to the last record.
  Status s = dest_->Append(buffer_);
  if (s.ok()) {
    s = sync_each_record_ ? dest_->Sync() : dest_->Flush();
  }
  if (!s.ok()) {
    status_ = s;
    return s;
  }
  bytes_written_ += buffer_.size();
  ++records_written_;
  return Status::OK();
}

Status Writer::Sync() {
  if (!status_.ok()) {
    return status_;
  }
  Status s = dest_->Sync();
  if (!s.ok()) {
    status_ = s;
  }
  return s;
}

}  // namespace lsm::wal
