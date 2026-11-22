#include "db/builder.h"

#include <memory>

#include "db/filename.h"
#include "memtable/memtable.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"

namespace lsm {

namespace {

// Re-opens a freshly written table and checks that it is readable and holds
// the expected number of entries.
Status VerifyTable(Env* env, const std::string& fname,
                   const TableOptions& table_options, uint64_t file_size,
                   uint64_t expected_entries) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = env->NewRandomAccessFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  std::shared_ptr<Table> table;
  s = Table::Open(table_options, std::move(file), file_size, nullptr, &table);
  if (!s.ok()) {
    return s;
  }
  uint64_t count = 0;
  auto it = table->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++count;
  }
  if (!it->status().ok()) {
    return it->status();
  }
  if (count != expected_entries) {
    return Status::Corruption("flushed table has wrong number of entries");
  }
  return Status::OK();
}

}  // namespace

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  const InternalKeyComparator& icmp, Iterator* iter,
                  uint64_t number, FileMetaData* meta) {
  *meta = FileMetaData();
  meta->number = number;

  iter->SeekToFirst();
  if (!iter->Valid()) {
    return iter->status();  // nothing to write
  }

  const std::string tmp_name = TempFileName(dbname, number);
  const std::string final_name = TableFileName(dbname, number);
  const TableOptions table_options =
      TableOptions::FromOptions(options, &icmp, /*internal_keys=*/true);

  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(tmp_name, &file);
  if (!s.ok()) {
    return s;
  }

  TableBuilder builder(table_options, file.get());
  meta->smallest.assign(iter->key().data(), iter->key().size());
  for (; iter->Valid(); iter->Next()) {
    const std::string_view key = iter->key();
    ParsedInternalKey parsed;
    if (!ParseInternalKey(key, &parsed)) {
      s = Status::Corruption("flush input contains a malformed internal key");
      break;
    }
    builder.Add(key, iter->value());
    if (!builder.status().ok()) {
      break;
    }
    meta->largest.assign(key.data(), key.size());
  }
  if (s.ok()) {
    s = iter->status();
  }

  if (s.ok()) {
    s = builder.Finish();
  } else {
    builder.Abandon();
  }
  if (s.ok()) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  file.reset();

  if (s.ok()) {
    meta->num_entries = builder.NumEntries();
    meta->file_size = builder.FileSize();
    s = VerifyTable(env, tmp_name, table_options, meta->file_size,
                    meta->num_entries);
  }
  if (s.ok()) {
    s = env->RenameFile(tmp_name, final_name);
  }

  if (!s.ok()) {
    env->RemoveFile(tmp_name);  // best effort; the error is what matters
    const uint64_t keep_number = meta->number;
    *meta = FileMetaData();
    meta->number = keep_number;
  }
  return s;
}

Status FlushMemTable(const std::string& dbname, Env* env,
                     const Options& options, const MemTable& mem,
                     uint64_t number, FileMetaData* meta) {
  auto iter = mem.NewIterator();
  return BuildTable(dbname, env, options, mem.internal_comparator(), iter.get(),
                    number, meta);
}

}  // namespace lsm
