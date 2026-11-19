// Sequential WAL reader used for crash recovery.
//
// Reading stops at the first record that cannot be trusted:
//   * a record cut short by the end of the file (torn write after a crash)
//     ends the log cleanly: status() stays OK and tail_truncated() is true;
//   * a checksum mismatch, unknown type or implausible length is reported as
//     Status::Corruption.
// In both cases every record before the damaged one has been returned.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/env.h"
#include "lsm/status.h"
#include "wal/log_format.h"

namespace lsm::wal {

class Reader {
 public:
  explicit Reader(SequentialFile* file,
                  size_t max_record_size = kDefaultMaxRecordSize);
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Reads the next record. Returns false at end of log or on error.
  bool ReadRecord(Record* record);

  const Status& status() const { return status_; }
  bool tail_truncated() const { return tail_truncated_; }
  // File offset just past the last record that was returned.
  uint64_t valid_bytes() const { return valid_bytes_; }
  uint64_t records_read() const { return records_read_; }

 private:
  // Reads up to n bytes into *out, stopping early only at end of file.
  Status ReadUpTo(size_t n, std::string* out);
  void ReportCorruption(const char* reason);

  SequentialFile* file_;
  size_t max_record_size_;
  Status status_;
  bool eof_ = false;
  bool tail_truncated_ = false;
  uint64_t valid_bytes_ = 0;
  uint64_t records_read_ = 0;
  std::unique_ptr<char[]> scratch_;
  std::string header_;
  std::string payload_;
};

}  // namespace lsm::wal
