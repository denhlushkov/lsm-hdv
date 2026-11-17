// File system abstraction (VFS). Allows fault injection in tests
// (design doc, section 6.1, level L5).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/status.h"

namespace lsm {

// Sequential, forward-only reads (used by WAL replay).
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  // Reads up to n bytes. *result may point into scratch (n bytes long).
  // Reaching end of file is not an error: *result is just shorter.
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

// Positional reads; must be safe for concurrent use (used by SSTables).
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result,
                      char* scratch) const = 0;
};

// Append-only output file.
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual Status Append(std::string_view data) = 0;
  // Push buffered data to the operating system.
  virtual Status Flush() = 0;
  // Push data to durable storage (fsync).
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

class Env {
 public:
  virtual ~Env() = default;

  // Process-wide environment backed by the real file system. Never deleted.
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(
      const std::string& fname, std::unique_ptr<RandomAccessFile>* result) = 0;
  // Creates (or truncates) a file for writing.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;
  // Opens a file for appending, creating it if needed.
  virtual Status NewAppendableFile(const std::string& fname,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  // Names (not paths) of the entries of a directory.
  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) = 0;
  virtual Status RemoveFile(const std::string& fname) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) = 0;
};

// Purely in-memory environment, used by tests and fault injection.
std::unique_ptr<Env> NewMemEnv();

// Helpers.
Status WriteStringToFile(Env* env, std::string_view data,
                         const std::string& fname, bool sync = false);
Status ReadFileToString(Env* env, const std::string& fname, std::string* data);

}  // namespace lsm
