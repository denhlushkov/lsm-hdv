// Names of the files inside a database directory.
//   <dbname>/000007.log   WAL segment #7
//   <dbname>/000012.sst   SSTable #12
//   <dbname>/000012.tmp   table being written (renamed when complete)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

enum class FileType { kWalFile, kTableFile, kTempFile };

std::string WalFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string TempFileName(const std::string& dbname, uint64_t number);

// Parses a file name (without directory). Returns false for foreign files.
bool ParseFileName(std::string_view filename, uint64_t* number,
                   FileType* type);

}  // namespace lsm
