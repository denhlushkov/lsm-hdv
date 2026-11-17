#include "db/filename.h"

#include <cstdio>
#include <limits>

namespace lsm {

namespace {

std::string MakeFileName(const std::string& dbname, uint64_t number,
                         const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

}  // namespace

std::string WalFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "sst");
}

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "tmp");
}

bool ParseFileName(std::string_view filename, uint64_t* number,
                   FileType* type) {
  uint64_t value = 0;
  size_t digits = 0;
  while (digits < filename.size() && filename[digits] >= '0' &&
         filename[digits] <= '9') {
    const uint64_t d = static_cast<uint64_t>(filename[digits] - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - d) / 10) {
      return false;  // overflow
    }
    value = value * 10 + d;
    ++digits;
  }
  if (digits == 0) {
    return false;
  }
  const std::string_view suffix = filename.substr(digits);
  if (suffix == ".log") {
    *type = FileType::kWalFile;
  } else if (suffix == ".sst") {
    *type = FileType::kTableFile;
  } else if (suffix == ".tmp") {
    *type = FileType::kTempFile;
  } else {
    return false;
  }
  *number = value;
  return true;
}

}  // namespace lsm
