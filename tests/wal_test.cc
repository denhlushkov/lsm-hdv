// Tests for the Write-Ahead Log (design doc: 4.1, U-05, FI-01, F-02).
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db/dbformat.h"
#include "memtable/memtable.h"
#include "test_util.h"
#include "util/coding.h"
#include "util/crc32.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"
#include "wal/wal_replay.h"

namespace lsm {
namespace {

using wal::Record;
using wal::RecordType;

class WalTest : public ::testing::Test {
 protected:
  WalTest() : env_(NewMemEnv()) {}

  void OpenWriter(bool sync = false) {
    ASSERT_OK(env_->NewWritableFile(kLog, &file_));
    writer_ = std::make_unique<wal::Writer>(file_.get(), sync);
  }

  std::string Contents() {
    std::string data;
    EXPECT_OK(ReadFileToString(env_.get(), kLog, &data));
    return data;
  }

  void Overwrite(const std::string& data) {
    ASSERT_OK(WriteStringToFile(env_.get(), data, kLog));
  }

  // Reads all records; also returns the reader's final state.
  std::vector<Record> ReadAll(Status* status = nullptr,
                              bool* truncated = nullptr) {
    std::unique_ptr<SequentialFile> file;
    EXPECT_OK(env_->NewSequentialFile(kLog, &file));
    wal::Reader reader(file.get());
    std::vector<Record> records;
    Record r;
    while (reader.ReadRecord(&r)) {
      records.push_back(r);
    }
    if (status != nullptr) *status = reader.status();
    if (truncated != nullptr) *truncated = reader.tail_truncated();
    return records;
  }

  const std::string kLog = "/wal/000001.log";
  std::unique_ptr<Env> env_;
  std::unique_ptr<WritableFile> file_;
  std::unique_ptr<wal::Writer> writer_;
};

TEST_F(WalTest, RecordLayoutMatchesSpecification) {
  std::string encoded;
  wal::EncodeRecord(RecordType::kPut, "key", "value", &encoded);
  ASSERT_EQ(encoded.size(), wal::kHeaderSize + 3 + 5);
  EXPECT_EQ(static_cast<uint8_t>(encoded[4]), 0x01);  // PUT
  EXPECT_EQ(DecodeFixed32(encoded.data() + 5), 3u);   // key length (LE)
  EXPECT_EQ(DecodeFixed32(encoded.data() + 9), 5u);   // value length (LE)
  EXPECT_EQ(encoded.substr(13), "keyvalue");
  // CRC32 covers everything after the checksum field.
  EXPECT_EQ(DecodeFixed32(encoded.data()),
            crc32::Value(encoded.data() + 4, encoded.size() - 4));

  std::string del;
  wal::EncodeRecord(RecordType::kDelete, "key", "", &del);
  EXPECT_EQ(static_cast<uint8_t>(del[4]), 0x02);  // DELETE
  EXPECT_EQ(DecodeFixed32(del.data() + 9), 0u);
}

TEST_F(WalTest, EmptyLogHasNoRecords) {
  OpenWriter();
  Status s;
  bool truncated = true;
  EXPECT_TRUE(ReadAll(&s, &truncated).empty());
  EXPECT_OK(s);
  EXPECT_FALSE(truncated);
}

TEST_F(WalTest, RoundTripManyRecords) {
  OpenWriter();
  Random rnd(17);
  std::vector<Record> written;
  for (int i = 0; i < 1000; ++i) {
    Record r;
    r.type = rnd.OneIn(5) ? RecordType::kDelete : RecordType::kPut;
    r.key = test::RandomBytes(&rnd, rnd.Uniform(50));
    if (r.type == RecordType::kPut) {
      // Occasionally a large value spanning several read chunks.
      r.value = test::RandomBytes(&rnd, rnd.OneIn(100) ? 100000 : rnd.Uniform(200));
    }
    ASSERT_OK(writer_->AddRecord(r.type, r.key, r.value));
    written.push_back(r);
  }
  EXPECT_EQ(writer_->records_written(), 1000u);
  EXPECT_EQ(writer_->bytes_written(), Contents().size());

  Status s;
  const auto read = ReadAll(&s);
  EXPECT_OK(s);
  ASSERT_EQ(read.size(), written.size());
  for (size_t i = 0; i < read.size(); ++i) {
    EXPECT_EQ(read[i].type, written[i].type) << i;
    EXPECT_EQ(read[i].key, written[i].key) << i;
    EXPECT_EQ(read[i].value, written[i].value) << i;
  }
}

TEST_F(WalTest, WriterRejectsInvalidRecords) {
  OpenWriter();
  EXPECT_TRUE(writer_->AddRecord(RecordType::kDelete, "k", "value")
                  .is_invalid_argument());
  EXPECT_TRUE(writer_->AddRecord(static_cast<RecordType>(7), "k", "v")
                  .is_invalid_argument());
  EXPECT_EQ(Contents().size(), 0u);  // nothing was written
}

// U-05: a single modified byte anywhere in a record is detected.
TEST_F(WalTest, DetectsCorruptedByteInEveryPosition) {
  OpenWriter();
  ASSERT_OK(writer_->AddPut("first", "value-1"));
  ASSERT_OK(writer_->AddPut("second", "value-2"));
  const std::string original = Contents();
  const size_t first_len = wal::kHeaderSize + 5 + 7;

  for (size_t pos = first_len; pos < original.size(); ++pos) {
    std::string damaged = original;
    damaged[pos] = static_cast<char>(damaged[pos] ^ 0x40);
    Overwrite(damaged);
    Status s;
    const auto records = ReadAll(&s);
    // The first record survives; the damaged second one is never returned.
    ASSERT_EQ(records.size(), 1u) << "byte " << pos;
    EXPECT_EQ(records[0].key, "first");
    // A damaged length field can also look like a torn tail; everything
    // else must be reported as corruption.
    const bool length_field = pos >= first_len + 5 && pos < first_len + 13;
    if (!length_field) {
      EXPECT_TRUE(s.is_corruption()) << "byte " << pos << ": " << s.to_string();
    }
  }
}

TEST_F(WalTest, StopsAtFirstCorruptRecord) {
  OpenWriter();
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK(writer_->AddPut("key" + std::to_string(i), "value"));
  }
  std::string data = Contents();
  const size_t record_len = data.size() / 10;
  data[4 * record_len + wal::kHeaderSize] ^= 0x01;  // corrupt record #4's key
  Overwrite(data);

  Status s;
  const auto records = ReadAll(&s);
  EXPECT_TRUE(s.is_corruption());
  ASSERT_EQ(records.size(), 4u);  // records 5..9 are not trusted either
  EXPECT_EQ(records.back().key, "key3");
}

// FI-01: a crash in the middle of writing a record (torn write).
TEST_F(WalTest, TornWriteRecoversEveryCompleteRecord) {
  OpenWriter();
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(writer_->AddPut("key" + std::to_string(i), "value"));
  }
  const std::string full = Contents();
  const size_t record_len = full.size() / 5;

  // Cut the file at every possible length inside the last record.
  for (size_t cut = 4 * record_len; cut < full.size(); ++cut) {
    Overwrite(full.substr(0, cut));
    Status s;
    bool truncated = false;
    const auto records = ReadAll(&s, &truncated);
    EXPECT_OK(s);
    EXPECT_EQ(records.size(), 4u) << "cut at " << cut;
    EXPECT_EQ(truncated, cut != 4 * record_len) << "cut at " << cut;
  }
}

TEST_F(WalTest, ImplausibleLengthIsCorruptionNotAllocation) {
  std::string data;
  wal::EncodeRecord(RecordType::kPut, "k", "v", &data);
  EncodeFixed32(data.data() + 5, 0xFFFFFFF0u);  // absurd key length
  Overwrite(data);
  Status s;
  EXPECT_TRUE(ReadAll(&s).empty());
  EXPECT_TRUE(s.is_corruption()) << s.to_string();
}

TEST_F(WalTest, SyncWalCallsFsyncForEveryRecord) {
  test::FaultInjectionEnv fenv(env_.get());
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(fenv.NewWritableFile(kLog, &file));
  wal::Writer synced(file.get(), /*sync_each_record=*/true);
  ASSERT_OK(synced.AddPut("a", "1"));
  ASSERT_OK(synced.AddDelete("a"));
  EXPECT_EQ(fenv.sync_calls(), 2u);

  std::unique_ptr<WritableFile> file2;
  ASSERT_OK(fenv.NewWritableFile("/wal/000002.log", &file2));
  wal::Writer unsynced(file2.get(), /*sync_each_record=*/false);
  ASSERT_OK(unsynced.AddPut("a", "1"));
  EXPECT_EQ(fenv.sync_calls(), 2u);
  ASSERT_OK(unsynced.Sync());
  EXPECT_EQ(fenv.sync_calls(), 3u);
}

TEST_F(WalTest, WriteErrorIsSticky) {
  test::FaultInjectionEnv fenv(env_.get());
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(fenv.NewWritableFile(kLog, &file));
  wal::Writer writer(file.get());
  ASSERT_OK(writer.AddPut("a", "1"));
  fenv.SetWriteBudget(5);  // the next record only half fits: disk full
  EXPECT_TRUE(writer.AddPut("b", "2").is_io_error());
  fenv.SetWriteBudget(-1);  // space is back, but the log tail is torn
  EXPECT_TRUE(writer.AddPut("c", "3").is_io_error());

  Status s;
  bool truncated = false;
  const auto records = ReadAll(&s, &truncated);
  EXPECT_OK(s);
  EXPECT_TRUE(truncated);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].key, "a");
}

// Crash recovery: the MemTable state is rebuilt from the log.
TEST_F(WalTest, ReplayRebuildsMemTable) {
  OpenWriter();
  ASSERT_OK(writer_->AddPut("a", "1"));
  ASSERT_OK(writer_->AddPut("b", "2"));
  ASSERT_OK(writer_->AddDelete("a"));
  ASSERT_OK(writer_->AddPut("c", "3"));
  ASSERT_OK(writer_->AddPut("b", "22"));

  InternalKeyComparator icmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(icmp);
  WalReplayResult result;
  ASSERT_OK(ReplayWalFile(env_.get(), kLog, mem.get(), 100, &result));
  EXPECT_EQ(result.records_applied, 5u);
  EXPECT_EQ(result.last_sequence, 104u);
  EXPECT_EQ(result.valid_bytes, Contents().size());
  EXPECT_FALSE(result.tail_truncated);

  std::string value;
  Status s;
  ASSERT_TRUE(mem->Get(LookupKey("a", kMaxSequenceNumber), &value, &s));
  EXPECT_TRUE(s.is_not_found());
  ASSERT_TRUE(mem->Get(LookupKey("b", kMaxSequenceNumber), &value, &s));
  EXPECT_EQ(value, "22");
  ASSERT_TRUE(mem->Get(LookupKey("c", kMaxSequenceNumber), &value, &s));
  EXPECT_EQ(value, "3");
  // Sequence numbers preserve the log order.
  ASSERT_TRUE(mem->Get(LookupKey("b", 101), &value, &s));
  EXPECT_EQ(value, "2");
}

TEST_F(WalTest, ReplayAfterCorruptionKeepsPrefix) {
  OpenWriter();
  ASSERT_OK(writer_->AddPut("a", "1"));
  ASSERT_OK(writer_->AddPut("b", "2"));
  std::string data = Contents();
  data.back() ^= 0x10;  // corrupt the last record's value
  Overwrite(data);

  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp);
  WalReplayResult result;
  const Status s = ReplayWalFile(env_.get(), kLog, &mem, 1, &result);
  EXPECT_TRUE(s.is_corruption());
  EXPECT_TRUE(result.stopped_at_corruption);
  EXPECT_EQ(result.records_applied, 1u);
  EXPECT_EQ(mem.NumEntries(), 1u);
}

// F-02 (light): random damage never crashes the reader, and every record
// it returns is one that was written.
TEST_F(WalTest, RandomDamageNeverCrashes) {
  OpenWriter();
  std::vector<std::string> keys;
  for (int i = 0; i < 50; ++i) {
    keys.push_back("key-" + std::to_string(i));
    ASSERT_OK(writer_->AddPut(keys.back(), std::string(i, 'v')));
  }
  const std::string original = Contents();
  Random rnd(4242);
  for (int trial = 0; trial < 300; ++trial) {
    std::string damaged = original;
    const int flips = 1 + static_cast<int>(rnd.Uniform(8));
    for (int f = 0; f < flips; ++f) {
      const size_t pos = rnd.Uniform(damaged.size());
      damaged[pos] = static_cast<char>(rnd.Uniform(256));
    }
    if (rnd.OneIn(3)) {
      damaged.resize(rnd.Uniform(damaged.size()));
    }
    Overwrite(damaged);
    const auto records = ReadAll();
    ASSERT_LE(records.size(), keys.size());
    for (size_t i = 0; i < records.size(); ++i) {
      EXPECT_EQ(records[i].key, keys[i]);
    }
  }
}

}  // namespace
}  // namespace lsm
