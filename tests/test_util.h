// Shared test helpers: random data, and a fault-injecting Env wrapper
// (design doc, level L5: Mock VFS).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lsm/env.h"
#include "lsm/status.h"
#include "util/random.h"

namespace lsm::test {

#define ASSERT_OK(expr)                                  \
  do {                                                   \
    const ::lsm::Status _s = (expr);                     \
    ASSERT_TRUE(_s.ok()) << _s.to_string();              \
  } while (0)

#define EXPECT_OK(expr)                                  \
  do {                                                   \
    const ::lsm::Status _s = (expr);                     \
    EXPECT_TRUE(_s.ok()) << _s.to_string();              \
  } while (0)

inline std::string RandomString(Random* rnd, size_t len) {
  std::string s(len, '\0');
  for (auto& c : s) {
    c = static_cast<char>(' ' + rnd->Uniform(95));
  }
  return s;
}

inline std::string RandomBytes(Random* rnd, size_t len) {
  std::string s(len, '\0');
  for (auto& c : s) {
    c = static_cast<char>(rnd->Uniform(256));
  }
  return s;
}

// Zero-padded decimal key, so lexicographic order == numeric order.
inline std::string NumberKey(uint64_t n, int width = 8) {
  std::string s = std::to_string(n);
  if (static_cast<int>(s.size()) < width) {
    s.insert(0, static_cast<size_t>(width) - s.size(), '0');
  }
  return "key" + s;
}

// Wraps another Env and injects failures:
//  * a write budget: once exhausted, Append writes the bytes that still fit
//    and fails with an ENOSPC-like IOError (disk full);
//  * failing Sync calls;
// and counts operations so tests can observe I/O behaviour.
class FaultInjectionEnv final : public Env {
 public:
  explicit FaultInjectionEnv(Env* target) : target_(target) {}

  // -1 = unlimited.
  void SetWriteBudget(int64_t bytes) { write_budget_.store(bytes); }
  void SetFailSync(bool fail) { fail_sync_.store(fail); }
  // Only files whose name ends with this suffix are affected ("" = all).
  void SetFaultSuffix(std::string suffix) { fault_suffix_ = std::move(suffix); }

  uint64_t sync_calls() const { return sync_calls_.load(); }
  uint64_t random_reads() const { return random_reads_.load(); }
  void ResetCounters() {
    sync_calls_ = 0;
    random_reads_ = 0;
  }

  Status NewSequentialFile(const std::string& f,
                           std::unique_ptr<SequentialFile>* r) override {
    return target_->NewSequentialFile(f, r);
  }
  Status NewRandomAccessFile(const std::string& f,
                             std::unique_ptr<RandomAccessFile>* r) override;
  Status NewWritableFile(const std::string& f,
                         std::unique_ptr<WritableFile>* r) override;
  Status NewAppendableFile(const std::string& f,
                           std::unique_ptr<WritableFile>* r) override;
  bool FileExists(const std::string& f) override {
    return target_->FileExists(f);
  }
  Status GetChildren(const std::string& d,
                     std::vector<std::string>* r) override {
    return target_->GetChildren(d, r);
  }
  Status RemoveFile(const std::string& f) override {
    return target_->RemoveFile(f);
  }
  Status CreateDir(const std::string& d) override {
    return target_->CreateDir(d);
  }
  Status GetFileSize(const std::string& f, uint64_t* s) override {
    return target_->GetFileSize(f, s);
  }
  Status RenameFile(const std::string& s, const std::string& t) override {
    return target_->RenameFile(s, t);
  }

 private:
  friend class FaultyWritableFile;
  friend class CountingRandomAccessFile;

  bool Affects(const std::string& fname) const {
    return fault_suffix_.empty() ||
           (fname.size() >= fault_suffix_.size() &&
            fname.compare(fname.size() - fault_suffix_.size(),
                          fault_suffix_.size(), fault_suffix_) == 0);
  }
  Status WrapWritable(const std::string& fname, Status s,
                      std::unique_ptr<WritableFile>* r);

  Env* target_;
  std::atomic<int64_t> write_budget_{-1};
  std::atomic<bool> fail_sync_{false};
  std::string fault_suffix_;
  std::atomic<uint64_t> sync_calls_{0};
  std::atomic<uint64_t> random_reads_{0};
};

class FaultyWritableFile final : public WritableFile {
 public:
  FaultyWritableFile(FaultInjectionEnv* env, std::unique_ptr<WritableFile> base,
                     bool affected)
      : env_(env), base_(std::move(base)), affected_(affected) {}

  Status Append(std::string_view data) override {
    if (affected_) {
      const int64_t budget = env_->write_budget_.load();
      if (budget >= 0 && static_cast<int64_t>(data.size()) > budget) {
        // Partial write, then "No space left on device".
        base_->Append(data.substr(0, static_cast<size_t>(budget)));
        env_->write_budget_.store(0);
        return Status::IOError("No space left on device (ENOSPC)");
      }
      if (budget >= 0) {
        env_->write_budget_.fetch_sub(static_cast<int64_t>(data.size()));
      }
    }
    return base_->Append(data);
  }
  Status Flush() override { return base_->Flush(); }
  Status Sync() override {
    env_->sync_calls_.fetch_add(1);
    if (affected_ && env_->fail_sync_.load()) {
      return Status::IOError("injected fsync failure (EIO)");
    }
    return base_->Sync();
  }
  Status Close() override { return base_->Close(); }

 private:
  FaultInjectionEnv* env_;
  std::unique_ptr<WritableFile> base_;
  bool affected_;
};

class CountingRandomAccessFile final : public RandomAccessFile {
 public:
  CountingRandomAccessFile(FaultInjectionEnv* env,
                           std::unique_ptr<RandomAccessFile> base)
      : env_(env), base_(std::move(base)) {}
  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    env_->random_reads_.fetch_add(1);
    return base_->Read(offset, n, result, scratch);
  }

 private:
  FaultInjectionEnv* env_;
  std::unique_ptr<RandomAccessFile> base_;
};

inline Status FaultInjectionEnv::NewRandomAccessFile(
    const std::string& f, std::unique_ptr<RandomAccessFile>* r) {
  std::unique_ptr<RandomAccessFile> base;
  Status s = target_->NewRandomAccessFile(f, &base);
  if (s.ok()) {
    *r = std::make_unique<CountingRandomAccessFile>(this, std::move(base));
  }
  return s;
}

inline Status FaultInjectionEnv::WrapWritable(const std::string& fname,
                                              Status s,
                                              std::unique_ptr<WritableFile>* r) {
  if (s.ok()) {
    *r = std::make_unique<FaultyWritableFile>(this, std::move(*r),
                                              Affects(fname));
  }
  return s;
}

inline Status FaultInjectionEnv::NewWritableFile(
    const std::string& f, std::unique_ptr<WritableFile>* r) {
  return WrapWritable(f, target_->NewWritableFile(f, r), r);
}

inline Status FaultInjectionEnv::NewAppendableFile(
    const std::string& f, std::unique_ptr<WritableFile>* r) {
  return WrapWritable(f, target_->NewAppendableFile(f, r), r);
}

}  // namespace lsm::test
