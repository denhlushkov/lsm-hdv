// Tests for SSTable building and reading (design doc: 4.3, U-04, F-01).
#include <atomic>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cache/lru_cache.h"
#include "sstable/format.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsm {
namespace {

using KVMap = std::map<std::string, std::string>;

class TableTest : public ::testing::Test {
 protected:
  TableTest() : base_env_(NewMemEnv()), env_(base_env_.get()) {}

  // Writes `data` as a table to kFile; returns the file size.
  uint64_t Build(const KVMap& data, TableOptions opts = TableOptions()) {
    std::unique_ptr<WritableFile> file;
    EXPECT_OK(env_.NewWritableFile(kFile, &file));
    TableBuilder builder(opts, file.get());
    for (const auto& [k, v] : data) {
      builder.Add(k, v);
    }
    EXPECT_OK(builder.Finish());
    EXPECT_EQ(builder.NumEntries(), data.size());
    EXPECT_OK(file->Close());
    last_num_blocks_ = builder.NumDataBlocks();
    return builder.FileSize();
  }

  Status Open(std::shared_ptr<Table>* table, LRUCache* cache = nullptr,
              TableOptions opts = TableOptions()) {
    uint64_t size = 0;
    Status s = env_.GetFileSize(kFile, &size);
    if (!s.ok()) return s;
    std::unique_ptr<RandomAccessFile> file;
    s = env_.NewRandomAccessFile(kFile, &file);
    if (!s.ok()) return s;
    return Table::Open(opts, std::move(file), size, cache, table);
  }

  std::string Contents() {
    std::string data;
    EXPECT_OK(ReadFileToString(&env_, kFile, &data));
    return data;
  }
  void Overwrite(const std::string& data) {
    ASSERT_OK(WriteStringToFile(&env_, data, kFile));
  }

  static KVMap MakeData(int n, size_t value_size = 100) {
    KVMap data;
    Random rnd(static_cast<uint64_t>(n));
    for (int i = 0; i < n; ++i) {
      data[test::NumberKey(static_cast<uint64_t>(i) * 3)] =
          test::RandomString(&rnd, value_size);
    }
    return data;
  }

  const std::string kFile = "/t/000001.sst";
  std::unique_ptr<Env> base_env_;
  test::FaultInjectionEnv env_;
  uint64_t last_num_blocks_ = 0;
};

TEST_F(TableTest, FooterLayoutAndMagicNumber) {
  const uint64_t size = Build(MakeData(10));
  const std::string contents = Contents();
  ASSERT_EQ(contents.size(), size);
  ASSERT_GE(contents.size(), Footer::kEncodedLength);
  // The last 8 bytes are the magic number 0xDB47534D5F4B5631 (LE).
  EXPECT_EQ(DecodeFixed64(contents.data() + contents.size() - 8),
            kTableMagicNumber);
  Footer footer;
  ASSERT_OK(footer.DecodeFrom(std::string_view(contents).substr(
      contents.size() - Footer::kEncodedLength)));
  EXPECT_GT(footer.filter_handle().size(), 0u);
  EXPECT_GT(footer.index_handle().size(), 0u);
  EXPECT_EQ(footer.index_handle().offset() + footer.index_handle().size() +
                kBlockTrailerSize + Footer::kEncodedLength,
            size);
}

TEST_F(TableTest, EmptyTable) {
  Build({});
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table));
  auto it = table->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
  std::string value;
  EXPECT_TRUE(table->Get("x", &value).is_not_found());
}

TEST_F(TableTest, RoundTripAcrossManyBlocks) {
  const KVMap data = MakeData(5000);
  Build(data);
  EXPECT_GT(last_num_blocks_, 100u);  // ~120 bytes/entry in 4 KB blocks

  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table));
  auto it = table->NewIterator();
  auto expected = data.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++expected) {
    ASSERT_NE(expected, data.end());
    ASSERT_EQ(it->key(), expected->first);
    ASSERT_EQ(it->value(), expected->second);
  }
  EXPECT_EQ(expected, data.end());
  EXPECT_OK(it->status());
}

TEST_F(TableTest, PointLookups) {
  const KVMap data = MakeData(2000);
  Build(data);
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table));
  std::string value;
  for (const auto& [k, v] : data) {
    ASSERT_OK(table->Get(k, &value));
    ASSERT_EQ(value, v);
  }
  // Keys between, before and after the stored ones.
  EXPECT_TRUE(table->Get(test::NumberKey(1), &value).is_not_found());
  EXPECT_TRUE(table->Get("a", &value).is_not_found());
  EXPECT_TRUE(table->Get("zzzz", &value).is_not_found());
}

TEST_F(TableTest, SeekPositionsAtFirstKeyGreaterOrEqual) {
  const KVMap data = MakeData(1000);
  Build(data);
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table));
  auto it = table->NewIterator();
  for (uint64_t i = 0; i < 3000; i += 7) {
    const std::string target = test::NumberKey(i);
    it->Seek(target);
    const auto expected = data.lower_bound(target);
    if (expected == data.end()) {
      EXPECT_FALSE(it->Valid());
    } else {
      ASSERT_TRUE(it->Valid());
      EXPECT_EQ(it->key(), expected->first);
    }
  }
}

TEST_F(TableTest, BloomFilterSkipsDiskReadsForAbsentKeys) {
  Build(MakeData(1000));
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table));
  ASSERT_TRUE(table->has_filter());
  env_.ResetCounters();
  std::string value;
  constexpr int kLookups = 2000;
  for (int i = 0; i < kLookups; ++i) {
    EXPECT_TRUE(table->Get("absent" + std::to_string(i), &value).is_not_found());
  }
  const TableReadStats stats = table->GetStats();
  // ~1% false positives: the vast majority never touch the file.
  EXPECT_GE(stats.filter_negatives, static_cast<uint64_t>(kLookups * 0.97));
  EXPECT_LE(env_.random_reads(), static_cast<uint64_t>(kLookups * 0.03));
}

TEST_F(TableTest, TableWithoutFilter) {
  TableOptions opts;
  opts.bloom_bits_per_key = 0;
  const KVMap data = MakeData(100);
  Build(data, opts);
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table, nullptr, opts));
  EXPECT_FALSE(table->has_filter());
  EXPECT_TRUE(table->KeyMayMatch("anything"));
  std::string value;
  ASSERT_OK(table->Get(data.begin()->first, &value));
  EXPECT_EQ(value, data.begin()->second);
}

TEST_F(TableTest, BlockCacheServesRepeatedReads) {
  const KVMap data = MakeData(1000);
  Build(data);
  LRUCache cache(1 << 20, 0);
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table, &cache));
  std::string value;
  const std::string key = data.begin()->first;
  ASSERT_OK(table->Get(key, &value));
  env_.ResetCounters();
  for (int i = 0; i < 100; ++i) {
    ASSERT_OK(table->Get(key, &value));
  }
  EXPECT_EQ(env_.random_reads(), 0u);  // all served from the cache
  const TableReadStats stats = table->GetStats();
  EXPECT_EQ(stats.block_reads, 1u);
  EXPECT_EQ(stats.cache_hits, 100u);
  EXPECT_GT(cache.TotalCharge(), 0u);
}

TEST_F(TableTest, OutOfOrderKeysAreRejected) {
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env_.NewWritableFile(kFile, &file));
  TableBuilder builder(TableOptions(), file.get());
  builder.Add("b", "1");
  builder.Add("a", "2");
  EXPECT_TRUE(builder.status().is_invalid_argument());
  EXPECT_TRUE(builder.Finish().is_invalid_argument());
}

TEST_F(TableTest, NotAnSSTableIsCorruption) {
  Overwrite(std::string(100, 'x'));
  std::shared_ptr<Table> table;
  Status s = Open(&table);
  EXPECT_TRUE(s.is_corruption()) << s.to_string();
  EXPECT_NE(s.message().find("magic"), std::string::npos);

  Overwrite("short");
  s = Open(&table);
  EXPECT_TRUE(s.is_corruption()) << s.to_string();
  EXPECT_EQ(table, nullptr);
}

TEST_F(TableTest, CorruptedFooterOrIndexIsDetected) {
  Build(MakeData(500));
  const std::string good = Contents();
  std::shared_ptr<Table> table;

  // Footer handle bytes (protected by the footer CRC).
  std::string bad = good;
  bad[bad.size() - Footer::kEncodedLength + 3] ^= 0x01;
  Overwrite(bad);
  EXPECT_TRUE(Open(&table).is_corruption());

  // Index block contents (protected by the block CRC).
  Footer footer;
  ASSERT_OK(footer.DecodeFrom(
      std::string_view(good).substr(good.size() - Footer::kEncodedLength)));
  bad = good;
  bad[footer.index_handle().offset() + 2] ^= 0x01;
  Overwrite(bad);
  EXPECT_TRUE(Open(&table).is_corruption());

  // Truncated file.
  Overwrite(good.substr(0, good.size() - 1));
  EXPECT_TRUE(Open(&table).is_corruption());
}

TEST_F(TableTest, CorruptedDataBlockIsReportedOnRead) {
  const KVMap data = MakeData(500);
  Build(data);
  std::string bad = Contents();
  bad[10] ^= 0x20;  // inside the first data block
  Overwrite(bad);

  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table));  // the footer/index are intact
  std::string value;
  EXPECT_TRUE(table->Get(data.begin()->first, &value).is_corruption());

  auto it = table->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().is_corruption());
}

// F-01 (light): random corruption anywhere in the file never crashes and
// never produces data that was not written.
TEST_F(TableTest, RandomCorruptionNeverCrashes) {
  const KVMap data = MakeData(300, 20);
  Build(data);
  const std::string good = Contents();
  Random rnd(777);
  for (int trial = 0; trial < 500; ++trial) {
    std::string bad = good;
    const int changes = 1 + static_cast<int>(rnd.Uniform(4));
    for (int c = 0; c < changes; ++c) {
      bad[rnd.Uniform(bad.size())] = static_cast<char>(rnd.Uniform(256));
    }
    Overwrite(bad);
    std::shared_ptr<Table> table;
    if (!Open(&table).ok()) {
      continue;
    }
    auto it = table->NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const auto found = data.find(std::string(it->key()));
      ASSERT_NE(found, data.end()) << "invented key in trial " << trial;
      ASSERT_EQ(found->second, it->value());
    }
  }
}

TEST_F(TableTest, IteratorKeepsTableAliveAndConcurrentReadsWork) {
  const KVMap data = MakeData(2000);
  Build(data);
  LRUCache cache(64 * 1024, 2);  // small: forces evictions
  std::shared_ptr<Table> table;
  ASSERT_OK(Open(&table, &cache));

  auto it = table->NewIterator();
  std::weak_ptr<Table> weak = table;
  table.reset();
  ASSERT_FALSE(weak.expired());  // the iterator holds the table
  size_t count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++count;
  }
  EXPECT_EQ(count, data.size());
  table = weak.lock();
  it.reset();

  std::vector<std::thread> readers;
  std::atomic<int> errors{0};
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      Random rnd(static_cast<uint64_t>(t) + 10);
      std::string value;
      for (int i = 0; i < 2000; ++i) {
        auto entry = data.begin();
        std::advance(entry, static_cast<long>(rnd.Uniform(data.size())));
        if (!table->Get(entry->first, &value).ok() || value != entry->second) {
          ++errors;
        }
      }
    });
  }
  for (auto& r : readers) {
    r.join();
  }
  EXPECT_EQ(errors.load(), 0);
}

}  // namespace
}  // namespace lsm
