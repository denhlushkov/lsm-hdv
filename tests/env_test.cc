// Tests for the Env implementations (real file system and in-memory).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

#include "lsm/env.h"
#include "test_util.h"

namespace lsm {
namespace {

// The same behavioural checks run against both Env implementations.
class EnvTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    if (GetParam()) {
      mem_env_ = NewMemEnv();
      env_ = mem_env_.get();
      dir_ = "/mem/envtest";
    } else {
      env_ = Env::Default();
      const auto stamp =
          std::chrono::steady_clock::now().time_since_epoch().count();
      dir_ = (std::filesystem::temp_directory_path() /
              ("lsm_env_test_" + std::to_string(stamp)))
                 .string();
    }
    ASSERT_OK(env_->CreateDir(dir_));
  }

  void TearDown() override {
    if (!GetParam()) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }

  std::unique_ptr<Env> mem_env_;
  Env* env_ = nullptr;
  std::string dir_;
};

TEST_P(EnvTest, WriteThenReadSequentiallyAndRandomly) {
  const std::string fname = dir_ + "/data.bin";
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(env_->NewWritableFile(fname, &w));
  ASSERT_OK(w->Append("hello "));
  ASSERT_OK(w->Append("world"));
  ASSERT_OK(w->Sync());
  ASSERT_OK(w->Close());

  uint64_t size = 0;
  ASSERT_OK(env_->GetFileSize(fname, &size));
  EXPECT_EQ(size, 11u);

  std::string contents;
  ASSERT_OK(ReadFileToString(env_, fname, &contents));
  EXPECT_EQ(contents, "hello world");

  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(env_->NewRandomAccessFile(fname, &r));
  char scratch[16];
  std::string_view result;
  ASSERT_OK(r->Read(6, 5, &result, scratch));
  EXPECT_EQ(result, "world");
  ASSERT_OK(r->Read(8, 10, &result, scratch));  // short read at EOF
  EXPECT_EQ(result, "rld");
  ASSERT_OK(r->Read(100, 4, &result, scratch));  // past EOF
  EXPECT_TRUE(result.empty());

  std::unique_ptr<SequentialFile> seq;
  ASSERT_OK(env_->NewSequentialFile(fname, &seq));
  ASSERT_OK(seq->Skip(6));
  ASSERT_OK(seq->Read(16, &result, scratch));
  EXPECT_EQ(result, "world");
}

TEST_P(EnvTest, AppendableFileKeepsExistingContents) {
  const std::string fname = dir_ + "/append.log";
  ASSERT_OK(WriteStringToFile(env_, "abc", fname));
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(env_->NewAppendableFile(fname, &w));
  ASSERT_OK(w->Append("def"));
  ASSERT_OK(w->Close());
  std::string contents;
  ASSERT_OK(ReadFileToString(env_, fname, &contents));
  EXPECT_EQ(contents, "abcdef");
}

TEST_P(EnvTest, DirectoryOperations) {
  ASSERT_OK(WriteStringToFile(env_, "1", dir_ + "/a.sst"));
  ASSERT_OK(WriteStringToFile(env_, "2", dir_ + "/b.log"));
  std::vector<std::string> children;
  ASSERT_OK(env_->GetChildren(dir_, &children));
  std::sort(children.begin(), children.end());
  EXPECT_EQ(children, (std::vector<std::string>{"a.sst", "b.log"}));

  ASSERT_OK(env_->RenameFile(dir_ + "/a.sst", dir_ + "/c.sst"));
  EXPECT_FALSE(env_->FileExists(dir_ + "/a.sst"));
  EXPECT_TRUE(env_->FileExists(dir_ + "/c.sst"));

  ASSERT_OK(env_->RemoveFile(dir_ + "/c.sst"));
  EXPECT_FALSE(env_->FileExists(dir_ + "/c.sst"));
  EXPECT_FALSE(env_->RemoveFile(dir_ + "/c.sst").ok());
}

TEST_P(EnvTest, OpeningMissingFileFails) {
  std::unique_ptr<SequentialFile> seq;
  const Status s = env_->NewSequentialFile(dir_ + "/missing", &seq);
  EXPECT_TRUE(s.is_not_found()) << s.to_string();
  std::unique_ptr<RandomAccessFile> r;
  EXPECT_FALSE(env_->NewRandomAccessFile(dir_ + "/missing", &r).ok());
  uint64_t size = 0;
  EXPECT_FALSE(env_->GetFileSize(dir_ + "/missing", &size).ok());
}

INSTANTIATE_TEST_SUITE_P(BothEnvs, EnvTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& param_info) {
                           return param_info.param ? "MemEnv" : "StdEnv";
                         });

TEST(MemEnvTest, RemovedFileStaysReadableThroughOpenHandle) {
  auto env = NewMemEnv();
  ASSERT_OK(WriteStringToFile(env.get(), "payload", "/f"));
  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(env->NewRandomAccessFile("/f", &r));
  ASSERT_OK(env->RemoveFile("/f"));
  char scratch[8];
  std::string_view result;
  ASSERT_OK(r->Read(0, 7, &result, scratch));
  EXPECT_EQ(result, "payload");
}

}  // namespace
}  // namespace lsm
