// Status: result of an operation (design doc, section 5).
#pragma once

#include <string>
#include <string_view>

namespace lsm {

enum class StatusCode {
  kOk = 0,
  kNotFound = 1,
  kCorruption = 2,
  kIOError = 3,
  kInvalidArgument = 4,
};

class Status {
 public:
  Status() : code_(StatusCode::kOk) {}
  Status(StatusCode code, std::string_view msg) : code_(code), msg_(msg) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg = {}) {
    return Status(StatusCode::kNotFound, msg);
  }
  static Status Corruption(std::string_view msg = {}) {
    return Status(StatusCode::kCorruption, msg);
  }
  static Status IOError(std::string_view msg = {}) {
    return Status(StatusCode::kIOError, msg);
  }
  static Status InvalidArgument(std::string_view msg = {}) {
    return Status(StatusCode::kInvalidArgument, msg);
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  bool is_not_found() const { return code_ == StatusCode::kNotFound; }
  bool is_corruption() const { return code_ == StatusCode::kCorruption; }
  bool is_io_error() const { return code_ == StatusCode::kIOError; }
  bool is_invalid_argument() const {
    return code_ == StatusCode::kInvalidArgument;
  }

  StatusCode code() const { return code_; }
  const std::string& message() const { return msg_; }

  // Human readable form, e.g. "Corruption: block checksum mismatch".
  std::string to_string() const;

 private:
  StatusCode code_;
  std::string msg_;
};

}  // namespace lsm
