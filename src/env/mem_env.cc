// In-memory Env. File handles share ownership of the file contents, so (as on
// POSIX) a removed file stays readable through handles that are still open.
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "lsm/env.h"

namespace lsm {

namespace {

struct MemFile {
  std::mutex mu;
  std::string data;
};

size_t CopyOut(MemFile& file, uint64_t offset, size_t n, char* scratch) {
  std::lock_guard<std::mutex> lock(file.mu);
  if (offset >= file.data.size()) {
    return 0;
  }
  const size_t available = file.data.size() - static_cast<size_t>(offset);
  const size_t len = std::min(n, available);
  if (len > 0) {
    std::memcpy(scratch, file.data.data() + offset, len);
  }
  return len;
}

class MemSequentialFile final : public SequentialFile {
 public:
  explicit MemSequentialFile(std::shared_ptr<MemFile> file)
      : file_(std::move(file)) {}

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const size_t len = CopyOut(*file_, pos_, n, scratch);
    pos_ += len;
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    pos_ += n;
    return Status::OK();
  }

 private:
  std::shared_ptr<MemFile> file_;
  uint64_t pos_ = 0;
};

class MemRandomAccessFile final : public RandomAccessFile {
 public:
  explicit MemRandomAccessFile(std::shared_ptr<MemFile> file)
      : file_(std::move(file)) {}

  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    const size_t len = CopyOut(*file_, offset, n, scratch);
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

 private:
  std::shared_ptr<MemFile> file_;
};

class MemWritableFile final : public WritableFile {
 public:
  explicit MemWritableFile(std::shared_ptr<MemFile> file)
      : file_(std::move(file)) {}

  Status Append(std::string_view data) override {
    if (closed_) {
      return Status::IOError("file is closed");
    }
    std::lock_guard<std::mutex> lock(file_->mu);
    file_->data.append(data.data(), data.size());
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override {
    closed_ = true;
    return Status::OK();
  }

 private:
  std::shared_ptr<MemFile> file_;
  bool closed_ = false;
};

class MemEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    auto file = Find(fname);
    if (!file) {
      result->reset();
      return Status::NotFound(fname);
    }
    *result = std::make_unique<MemSequentialFile>(std::move(file));
    return Status::OK();
  }

  Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) override {
    auto file = Find(fname);
    if (!file) {
      result->reset();
      return Status::NotFound(fname);
    }
    *result = std::make_unique<MemRandomAccessFile>(std::move(file));
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    auto file = std::make_shared<MemFile>();
    {
      std::lock_guard<std::mutex> lock(mu_);
      files_[fname] = file;
    }
    *result = std::make_unique<MemWritableFile>(std::move(file));
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    std::shared_ptr<MemFile> file;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto& slot = files_[fname];
      if (!slot) {
        slot = std::make_shared<MemFile>();
      }
      file = slot;
    }
    *result = std::make_unique<MemWritableFile>(std::move(file));
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    std::lock_guard<std::mutex> lock(mu_);
    return files_.count(fname) > 0 || dirs_.count(fname) > 0;
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    result->clear();
    const std::string prefix = dir + "/";
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [name, file] : files_) {
      if (name.size() > prefix.size() &&
          name.compare(0, prefix.size(), prefix) == 0) {
        const std::string rest = name.substr(prefix.size());
        if (rest.find('/') == std::string::npos) {
          result->push_back(rest);
        }
      }
    }
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (files_.erase(fname) == 0) {
      return Status::NotFound(fname);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    std::lock_guard<std::mutex> lock(mu_);
    dirs_.insert(dirname);
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    auto file = Find(fname);
    if (!file) {
      *size = 0;
      return Status::NotFound(fname);
    }
    std::lock_guard<std::mutex> lock(file->mu);
    *size = file->data.size();
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = files_.find(src);
    if (it == files_.end()) {
      return Status::NotFound(src);
    }
    auto file = it->second;
    files_.erase(it);
    files_[target] = std::move(file);
    return Status::OK();
  }

 private:
  std::shared_ptr<MemFile> Find(const std::string& fname) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = files_.find(fname);
    return it == files_.end() ? nullptr : it->second;
  }

  std::mutex mu_;
  std::map<std::string, std::shared_ptr<MemFile>> files_;
  std::set<std::string> dirs_;
};

}  // namespace

std::unique_ptr<Env> NewMemEnv() { return std::make_unique<MemEnv>(); }

}  // namespace lsm
