// Append-only WAL writer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/env.h"
#include "lsm/status.h"
#include "wal/log_format.h"

namespace lsm::wal {

class Writer {
 public:
  // dest must outlive the writer. With sync_each_record every record is
  // fsync'ed before AddRecord returns (Options::sync_wal); otherwise it is
  // only flushed to the operating system.
  explicit Writer(WritableFile* dest, bool sync_each_record = false);
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  Status AddRecord(RecordType type, std::string_view key,
                   std::string_view value);
  Status AddPut(std::string_view key, std::string_view value) {
    return AddRecord(RecordType::kPut, key, value);
  }
  Status AddDelete(std::string_view key) {
    return AddRecord(RecordType::kDelete, key, {});
  }

  Status Sync();

  uint64_t bytes_written() const { return bytes_written_; }
  uint64_t records_written() const { return records_written_; }

 private:
  WritableFile* dest_;
  bool sync_each_record_;
  // After a failed write the log may end with a partial record; anything
  // appended after it would be unreachable on replay, so errors are sticky.
  Status status_;
  std::string buffer_;
  uint64_t bytes_written_ = 0;
  uint64_t records_written_ = 0;
};

}  // namespace lsm::wal
