// Env backed by the real file system: C stdio for file contents and
// std::filesystem for directory operations. Portable across POSIX and Windows.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>

#include "lsm/env.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lsm {

namespace {

namespace fs = std::filesystem;

int SeekFile(std::FILE* f, uint64_t offset, int whence) {
#if defined(_WIN32)
  return _fseeki64(f, static_cast<__int64>(offset), whence);
#else
  return fseeko(f, static_cast<off_t>(offset), whence);
#endif
}

int SyncFile(std::FILE* f) {
#if defined(_WIN32)
  return _commit(_fileno(f));
#else
  return ::fsync(fileno(f));
#endif
}

Status ErrnoError(const std::string& context, int err) {
  return Status::IOError(context + ": " + std::strerror(err));
}

Status FsError(const std::string& context, const std::error_code& ec) {
  return Status::IOError(context + ": " + ec.message());
}

class StdSequentialFile final : public SequentialFile {
 public:
  StdSequentialFile(std::string fname, std::FILE* f)
      : fname_(std::move(fname)), file_(f) {}
  ~StdSequentialFile() override { std::fclose(file_); }

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const size_t r = std::fread(scratch, 1, n, file_);
    *result = std::string_view(scratch, r);
    if (r < n && std::ferror(file_)) {
      std::clearerr(file_);
      return ErrnoError(fname_, errno);
    }
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (SeekFile(file_, n, SEEK_CUR) != 0) {
      return ErrnoError(fname_, errno);
    }
    return Status::OK();
  }

 private:
  std::string fname_;
  std::FILE* file_;
};

class StdRandomAccessFile final : public RandomAccessFile {
 public:
  StdRandomAccessFile(std::string fname, std::FILE* f)
      : fname_(std::move(fname)), file_(f) {}
  ~StdRandomAccessFile() override { std::fclose(file_); }

  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    // stdio has a single file position, so positional reads are serialized.
    std::lock_guard<std::mutex> lock(mu_);
    if (SeekFile(file_, offset, SEEK_SET) != 0) {
      *result = {};
      return ErrnoError(fname_, errno);
    }
    const size_t r = std::fread(scratch, 1, n, file_);
    *result = std::string_view(scratch, r);
    if (r < n && std::ferror(file_)) {
      std::clearerr(file_);
      return ErrnoError(fname_, errno);
    }
    std::clearerr(file_);
    return Status::OK();
  }

 private:
  std::string fname_;
  mutable std::mutex mu_;
  std::FILE* file_;
};

class StdWritableFile final : public WritableFile {
 public:
  StdWritableFile(std::string fname, std::FILE* f)
      : fname_(std::move(fname)), file_(f) {}
  ~StdWritableFile() override {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  Status Append(std::string_view data) override {
    if (file_ == nullptr) {
      return Status::IOError(fname_ + ": file is closed");
    }
    if (data.empty()) {
      return Status::OK();
    }
    if (std::fwrite(data.data(), 1, data.size(), file_) != data.size()) {
      return ErrnoError(fname_, errno);
    }
    return Status::OK();
  }

  Status Flush() override {
    if (file_ == nullptr) {
      return Status::IOError(fname_ + ": file is closed");
    }
    if (std::fflush(file_) != 0) {
      return ErrnoError(fname_, errno);
    }
    return Status::OK();
  }

  Status Sync() override {
    Status s = Flush();
    if (s.ok() && SyncFile(file_) != 0) {
      s = ErrnoError(fname_, errno);
    }
    return s;
  }

  Status Close() override {
    if (file_ == nullptr) {
      return Status::OK();
    }
    const int r = std::fclose(file_);
    file_ = nullptr;
    if (r != 0) {
      return ErrnoError(fname_, errno);
    }
    return Status::OK();
  }

 private:
  std::string fname_;
  std::FILE* file_;
};

class StdEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    std::FILE* f = std::fopen(fname.c_str(), "rb");
    if (f == nullptr) {
      result->reset();
      return OpenError(fname);
    }
    *result = std::make_unique<StdSequentialFile>(fname, f);
    return Status::OK();
  }

  Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) override {
    std::FILE* f = std::fopen(fname.c_str(), "rb");
    if (f == nullptr) {
      result->reset();
      return OpenError(fname);
    }
    *result = std::make_unique<StdRandomAccessFile>(fname, f);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    return OpenForWrite(fname, "wb", result);
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    return OpenForWrite(fname, "ab", result);
  }

  bool FileExists(const std::string& fname) override {
    std::error_code ec;
    return fs::exists(fs::path(fname), ec);
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    result->clear();
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) {
      return FsError(dir, ec);
    }
    for (const auto& entry : it) {
      result->push_back(entry.path().filename().string());
    }
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    std::error_code ec;
    if (!fs::remove(fs::path(fname), ec)) {
      return ec ? FsError(fname, ec)
                : Status::IOError(fname + ": no such file");
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    std::error_code ec;
    fs::create_directories(fs::path(dirname), ec);
    if (ec) {
      return FsError(dirname, ec);
    }
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    std::error_code ec;
    const auto n = fs::file_size(fs::path(fname), ec);
    if (ec) {
      *size = 0;
      return FsError(fname, ec);
    }
    *size = static_cast<uint64_t>(n);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    std::error_code ec;
    fs::rename(fs::path(src), fs::path(target), ec);
    if (ec) {
      return FsError(src, ec);
    }
    return Status::OK();
  }

 private:
  static Status OpenError(const std::string& fname) {
    const int err = errno;
    if (err == ENOENT) {
      return Status::NotFound(fname);
    }
    return ErrnoError(fname, err);
  }

  static Status OpenForWrite(const std::string& fname, const char* mode,
                             std::unique_ptr<WritableFile>* result) {
    std::FILE* f = std::fopen(fname.c_str(), mode);
    if (f == nullptr) {
      result->reset();
      return ErrnoError(fname, errno);
    }
    *result = std::make_unique<StdWritableFile>(fname, f);
    return Status::OK();
  }
};

}  // namespace

Env* Env::Default() {
  static StdEnv env;
  return &env;
}

}  // namespace lsm
