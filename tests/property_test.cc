// Property-based tests with RapidCheck (design doc, level L2).
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "cache/lru_cache.h"
#include "db/dbformat.h"
#include "filter/bloom_filter.h"
#include "memtable/memtable.h"
#include "sstable/block.h"
#include "sstable/block_builder.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "util/coding.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace lsm {
namespace {

// Operation on a single small key space, so keys collide often.
struct Op {
  bool is_delete;
  int key;
  std::string value;
};

}  // namespace
}  // namespace lsm

namespace rc {
template <>
struct Arbitrary<lsm::Op> {
  static Gen<lsm::Op> arbitrary() {
    return gen::build<lsm::Op>(
        // Deletes are 1 in 4 operations.
        gen::set(&lsm::Op::is_delete, gen::element(true, false, false, false)),
        gen::set(&lsm::Op::key, gen::inRange(0, 20)),
        gen::set(&lsm::Op::value, gen::arbitrary<std::string>()));
  }
};
}  // namespace rc

namespace lsm {
namespace {

RC_GTEST_PROP(CodingProperty, VarintRoundTrip,
              (const std::vector<uint64_t>& values)) {
  std::string buf;
  for (uint64_t v : values) {
    PutVarint64(&buf, v);
  }
  std::string_view input(buf);
  for (uint64_t expected : values) {
    uint64_t actual = 0;
    RC_ASSERT(GetVarint64(&input, &actual));
    RC_ASSERT(actual == expected);
  }
  RC_ASSERT(input.empty());
}

// P-01: decodeBlock(encodeBlock(entries)) == entries.
RC_GTEST_PROP(BlockProperty, P01_EncodeDecodeRoundTrip,
              (const std::map<std::string, std::string>& entries)) {
  const int restart_interval = *rc::gen::inRange(1, 33);
  BlockBuilder builder(restart_interval, BytewiseComparator());
  for (const auto& [k, v] : entries) {
    builder.Add(k, v);
  }
  auto block = std::make_shared<Block>(std::string(builder.Finish()));
  RC_ASSERT(block->ok());

  std::map<std::string, std::string> decoded;
  auto it = NewBlockIterator(block, BytewiseComparator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    decoded.emplace(std::string(it->key()), std::string(it->value()));
  }
  RC_ASSERT(it->status().ok());
  RC_ASSERT(decoded == entries);

  // Seek agrees with std::map::lower_bound for arbitrary targets.
  const auto target = *rc::gen::arbitrary<std::string>();
  it->Seek(target);
  const auto expected = entries.lower_bound(target);
  if (expected == entries.end()) {
    RC_ASSERT(!it->Valid());
  } else {
    RC_ASSERT(it->Valid());
    RC_ASSERT(std::string(it->key()) == expected->first);
  }
}

// P-02: if the last operation on k was Delete(k), Get(k) is NotFound.
RC_GTEST_PROP(MemTableProperty, P02_ReadAfterDelete,
              (const std::vector<Op>& ops)) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(icmp);
  std::map<int, const Op*> last_op;
  SequenceNumber seq = 1;
  for (const Op& op : ops) {
    const std::string key = "k" + std::to_string(op.key);
    if (op.is_delete) {
      RC_ASSERT(mem->Delete(seq++, key).ok());
    } else {
      RC_ASSERT(mem->Put(seq++, key, op.value).ok());
    }
    last_op[op.key] = &op;
  }
  for (int k = 0; k <= 20; ++k) {
    std::string value;
    Status s;
    const bool found =
        mem->Get(LookupKey("k" + std::to_string(k), kMaxSequenceNumber),
                 &value, &s);
    const auto it = last_op.find(k);
    if (it == last_op.end()) {
      RC_ASSERT(!found);
    } else if (it->second->is_delete) {
      RC_ASSERT(found);
      RC_ASSERT(s.is_not_found());
    } else {
      RC_ASSERT(found);
      RC_ASSERT(s.ok());
      RC_ASSERT(value == it->second->value);
    }
  }
}

// P-03: iteration yields strictly increasing keys (MemTable and SSTable).
RC_GTEST_PROP(IteratorProperty, P03_StrictlyIncreasingKeys,
              (const std::vector<Op>& ops)) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(icmp);
  SequenceNumber seq = 1;
  for (const Op& op : ops) {
    const std::string key = "k" + std::to_string(op.key);
    RC_ASSERT((op.is_delete ? mem->Delete(seq++, key)
                            : mem->Put(seq++, key, op.value))
                  .ok());
  }

  // Flush to an in-memory SSTable as well.
  auto env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  RC_ASSERT(env->NewWritableFile("/t.sst", &file).ok());
  TableOptions topts;
  topts.comparator = &icmp;
  topts.filter_on_user_key = true;
  topts.block_size = 64;  // many tiny blocks
  TableBuilder builder(topts, file.get());
  auto mit = mem->NewIterator();
  std::string prev;
  bool first = true;
  size_t count = 0;
  for (mit->SeekToFirst(); mit->Valid(); mit->Next()) {
    if (!first) {
      RC_ASSERT(icmp.Compare(prev, mit->key()) < 0);
    }
    prev.assign(mit->key());
    first = false;
    builder.Add(mit->key(), mit->value());
    ++count;
  }
  RC_ASSERT(count == ops.size());
  RC_ASSERT(builder.Finish().ok());

  uint64_t size = 0;
  RC_ASSERT(env->GetFileSize("/t.sst", &size).ok());
  std::unique_ptr<RandomAccessFile> rfile;
  RC_ASSERT(env->NewRandomAccessFile("/t.sst", &rfile).ok());
  std::shared_ptr<Table> table;
  RC_ASSERT(Table::Open(topts, std::move(rfile), size, nullptr, &table).ok());
  auto tit = table->NewIterator();
  first = true;
  size_t table_count = 0;
  for (tit->SeekToFirst(); tit->Valid(); tit->Next()) {
    if (!first) {
      RC_ASSERT(icmp.Compare(prev, tit->key()) < 0);
    }
    prev.assign(tit->key());
    first = false;
    ++table_count;
  }
  RC_ASSERT(tit->status().ok());
  RC_ASSERT(table_count == count);
}

// SSTable round trip: every written pair is readable by Get and by scan.
RC_GTEST_PROP(TableProperty, BuildReadRoundTrip,
              (const std::map<std::string, std::string>& data)) {
  auto env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  RC_ASSERT(env->NewWritableFile("/t.sst", &file).ok());
  TableOptions opts;
  opts.block_size = static_cast<size_t>(*rc::gen::inRange(16, 4096));
  opts.block_restart_interval = *rc::gen::inRange(1, 20);
  TableBuilder builder(opts, file.get());
  for (const auto& [k, v] : data) {
    builder.Add(k, v);
  }
  RC_ASSERT(builder.Finish().ok());

  std::unique_ptr<RandomAccessFile> rfile;
  RC_ASSERT(env->NewRandomAccessFile("/t.sst", &rfile).ok());
  std::shared_ptr<Table> table;
  LRUCache cache(8192, 0);
  RC_ASSERT(Table::Open(opts, std::move(rfile), builder.FileSize(), &cache,
                        &table)
                .ok());
  std::string value;
  for (const auto& [k, v] : data) {
    RC_ASSERT(table->Get(k, &value).ok());
    RC_ASSERT(value == v);
  }
  std::map<std::string, std::string> scanned;
  auto it = table->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    scanned.emplace(std::string(it->key()), std::string(it->value()));
  }
  RC_ASSERT(scanned == data);
}

// WAL round trip for arbitrary binary keys and values.
RC_GTEST_PROP(WalProperty, WriteReadRoundTrip, (const std::vector<Op>& ops)) {
  auto env = NewMemEnv();
  std::unique_ptr<WritableFile> file;
  RC_ASSERT(env->NewWritableFile("/w.log", &file).ok());
  wal::Writer writer(file.get());
  for (const Op& op : ops) {
    const std::string key = "key-" + std::to_string(op.key);
    RC_ASSERT((op.is_delete ? writer.AddDelete(key) : writer.AddPut(key, op.value))
                  .ok());
  }
  std::unique_ptr<SequentialFile> rfile;
  RC_ASSERT(env->NewSequentialFile("/w.log", &rfile).ok());
  wal::Reader reader(rfile.get());
  wal::Record rec;
  size_t i = 0;
  while (reader.ReadRecord(&rec)) {
    RC_ASSERT(i < ops.size());
    RC_ASSERT((rec.type == wal::RecordType::kDelete) == ops[i].is_delete);
    RC_ASSERT(rec.key == "key-" + std::to_string(ops[i].key));
    RC_ASSERT(rec.value == (ops[i].is_delete ? std::string() : ops[i].value));
    ++i;
  }
  RC_ASSERT(reader.status().ok());
  RC_ASSERT(i == ops.size());
}

// Bloom filter: never a false negative for any key set.
RC_GTEST_PROP(BloomProperty, NoFalseNegatives,
              (const std::set<std::string>& keys)) {
  const int bits = *rc::gen::inRange(1, 25);
  BloomFilterPolicy policy(bits);
  std::vector<std::string_view> views(keys.begin(), keys.end());
  std::string filter;
  policy.CreateFilter(views, &filter);
  for (const auto& k : keys) {
    RC_ASSERT(BloomFilterPolicy::KeyMayMatch(k, filter));
  }
}

// LRU cache: usage never exceeds capacity and the most recent insert of a
// fitting entry is always retrievable.
RC_GTEST_PROP(LRUCacheProperty, BoundedUsageAndRecentEntryPresent,
              (const std::vector<std::pair<uint8_t, uint8_t>>& inserts)) {
  const size_t capacity = static_cast<size_t>(*rc::gen::inRange(1, 200));
  LRUCache cache(capacity, 0);
  for (const auto& [key, charge] : inserts) {
    const std::string k = std::to_string(key % 32);
    cache.Insert(k, std::make_shared<int>(key), charge);
    RC_ASSERT(cache.TotalCharge() <= capacity);
    if (charge <= capacity) {
      auto v = cache.LookupAs<int>(k);
      RC_ASSERT(v != nullptr);
      RC_ASSERT(*v == static_cast<int>(key));
    }
  }
}

}  // namespace
}  // namespace lsm
