#include "lsm/status.h"

namespace lsm {

std::string Status::to_string() const {
  const char* prefix = "Unknown";
  switch (code_) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotFound:
      prefix = "NotFound";
      break;
    case StatusCode::kCorruption:
      prefix = "Corruption";
      break;
    case StatusCode::kIOError:
      prefix = "IO error";
      break;
    case StatusCode::kInvalidArgument:
      prefix = "Invalid argument";
      break;
  }
  std::string result(prefix);
  if (!msg_.empty()) {
    result.append(": ");
    result.append(msg_);
  }
  return result;
}

}  // namespace lsm
