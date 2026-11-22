// Tests for flushing a MemTable into an SSTable (design doc: 3.1, FI-02).
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "db/builder.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "memtable/memtable.h"
#include "sstable/table.h"
#include "test_util.h"

namespace lsm {
namespace {

class BuilderTest : public ::testing::Test {
 protected:
  BuilderTest()
      : base_env_(NewMemEnv()),
        env_(base_env_.get()),
        icmp_(BytewiseComparator()),
        mem_(std::make_shared<MemTable>(icmp_)) {
    options_.env = &env_;
    EXPECT_OK(env_.CreateDir(kDb));
  }

  std::shared_ptr<Table> OpenTable(const FileMetaData& meta) {
    const std::string fname = TableFileName(kDb, meta.number);
    std::unique_ptr<RandomAccessFile> file;
    EXPECT_OK(env_.NewRandomAccessFile(fname, &file));
    std::shared_ptr<Table> table;
    EXPECT_OK(Table::Open(TableOptions::FromOptions(options_, &icmp_, true),
                          std::move(file), meta.file_size, nullptr, &table));
    return table;
  }

  const std::string kDb = "/db";
  std::unique_ptr<Env> base_env_;
  test::FaultInjectionEnv env_;
  InternalKeyComparator icmp_;
  std::shared_ptr<MemTable> mem_;
  Options options_;
};

TEST_F(BuilderTest, FlushWritesAllVersionsAndTombstones) {
  SequenceNumber seq = 1;
  for (int i = 0; i < 1000; ++i) {
    ASSERT_OK(mem_->Put(seq++, test::NumberKey(i), "value" + std::to_string(i)));
  }
  ASSERT_OK(mem_->Put(seq++, test::NumberKey(5), "updated"));
  ASSERT_OK(mem_->Delete(seq++, test::NumberKey(7)));
  mem_->MarkImmutable();

  FileMetaData meta;
  ASSERT_OK(FlushMemTable(kDb, &env_, options_, *mem_, 3, &meta));
  EXPECT_EQ(meta.number, 3u);
  EXPECT_EQ(meta.num_entries, 1002u);
  EXPECT_GT(meta.file_size, 0u);
  EXPECT_EQ(ExtractUserKey(meta.smallest), test::NumberKey(0));
  EXPECT_EQ(ExtractUserKey(meta.largest), test::NumberKey(999));
  EXPECT_TRUE(env_.FileExists(TableFileName(kDb, 3)));
  EXPECT_FALSE(env_.FileExists(TempFileName(kDb, 3)));

  // The table iterates in the same internal-key order as the MemTable.
  auto table = OpenTable(meta);
  ASSERT_NE(table, nullptr);
  auto mem_iter = mem_->NewIterator();
  auto table_iter = table->NewIterator();
  mem_iter->SeekToFirst();
  table_iter->SeekToFirst();
  size_t n = 0;
  while (mem_iter->Valid()) {
    ASSERT_TRUE(table_iter->Valid());
    ASSERT_EQ(table_iter->key(), mem_iter->key());
    ASSERT_EQ(table_iter->value(), mem_iter->value());
    mem_iter->Next();
    table_iter->Next();
    ++n;
  }
  EXPECT_FALSE(table_iter->Valid());
  EXPECT_EQ(n, 1002u);

  // Point lookup through the bloom filter on user keys finds the newest
  // version first.
  std::string found_key;
  std::string found_value;
  const LookupKey lk(test::NumberKey(5), kMaxSequenceNumber);
  ASSERT_OK(table->InternalGet(lk.internal_key(),
                               [&](std::string_view k, std::string_view v) {
                                 found_key.assign(k);
                                 found_value.assign(v);
                               }));
  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(found_key, &parsed));
  EXPECT_EQ(parsed.user_key, test::NumberKey(5));
  EXPECT_EQ(found_value, "updated");

  const LookupKey deleted(test::NumberKey(7), kMaxSequenceNumber);
  ASSERT_OK(table->InternalGet(deleted.internal_key(),
                               [&](std::string_view k, std::string_view) {
                                 found_key.assign(k);
                               }));
  ASSERT_TRUE(ParseInternalKey(found_key, &parsed));
  EXPECT_EQ(parsed.type, kTypeDeletion);

  const LookupKey absent("nope", kMaxSequenceNumber);
  EXPECT_FALSE(table->KeyMayMatch(absent.internal_key()));
}

TEST_F(BuilderTest, EmptyMemTableProducesNoFile) {
  FileMetaData meta;
  ASSERT_OK(FlushMemTable(kDb, &env_, options_, *mem_, 9, &meta));
  EXPECT_EQ(meta.file_size, 0u);
  EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 9)));
}

// FI-02: the disk fills up during the flush.
TEST_F(BuilderTest, DiskFullDuringFlushReportsErrorAndKeepsData) {
  for (int i = 0; i < 2000; ++i) {
    ASSERT_OK(mem_->Put(static_cast<SequenceNumber>(i + 1), test::NumberKey(i),
                        std::string(100, 'x')));
  }
  mem_->MarkImmutable();
  env_.SetFaultSuffix(".tmp");
  env_.SetWriteBudget(50 * 1024);  // the table needs ~250 KB

  FileMetaData meta;
  const Status s = FlushMemTable(kDb, &env_, options_, *mem_, 4, &meta);
  EXPECT_TRUE(s.is_io_error()) << s.to_string();
  EXPECT_NE(s.message().find("ENOSPC"), std::string::npos);
  EXPECT_EQ(meta.file_size, 0u);
  // No partial table is left behind.
  EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 4)));
  EXPECT_FALSE(env_.FileExists(TempFileName(kDb, 4)));
  // The MemTable still holds everything, so a retry can succeed.
  EXPECT_EQ(mem_->NumEntries(), 2000u);
  std::string value;
  Status get_status;
  ASSERT_TRUE(mem_->Get(LookupKey(test::NumberKey(1999), kMaxSequenceNumber),
                        &value, &get_status));
  EXPECT_OK(get_status);

  env_.SetWriteBudget(-1);  // space freed
  ASSERT_OK(FlushMemTable(kDb, &env_, options_, *mem_, 4, &meta));
  EXPECT_EQ(meta.num_entries, 2000u);
  EXPECT_TRUE(env_.FileExists(TableFileName(kDb, 4)));
}

TEST_F(BuilderTest, FsyncFailureDuringFlushIsReported) {
  ASSERT_OK(mem_->Put(1, "k", "v"));
  env_.SetFaultSuffix(".tmp");
  env_.SetFailSync(true);
  FileMetaData meta;
  const Status s = FlushMemTable(kDb, &env_, options_, *mem_, 5, &meta);
  EXPECT_TRUE(s.is_io_error()) << s.to_string();
  EXPECT_FALSE(env_.FileExists(TableFileName(kDb, 5)));
  EXPECT_FALSE(env_.FileExists(TempFileName(kDb, 5)));
}

}  // namespace
}  // namespace lsm
