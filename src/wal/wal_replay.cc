#include "wal/wal_replay.h"

#include <memory>

#include "memtable/memtable.h"
#include "wal/log_reader.h"

namespace lsm {

Status ReplayWal(SequentialFile* file, MemTable* mem,
                 SequenceNumber first_sequence, WalReplayResult* result) {
  *result = WalReplayResult();
  if (first_sequence == 0) {
    return Status::InvalidArgument("sequence numbers start at 1");
  }
  result->last_sequence = first_sequence - 1;

  wal::Reader reader(file);
  wal::Record record;
  SequenceNumber seq = first_sequence;
  while (reader.ReadRecord(&record)) {
    const ValueType type = record.type == wal::RecordType::kPut
                               ? kTypeValue
                               : kTypeDeletion;
    Status s = mem->Add(seq, type, record.key, record.value);
    if (!s.ok()) {
      return s;
    }
    result->last_sequence = seq;
    ++seq;
    ++result->records_applied;
  }
  result->valid_bytes = reader.valid_bytes();
  result->tail_truncated = reader.tail_truncated();
  result->stopped_at_corruption = reader.status().is_corruption();
  return reader.status();
}

Status ReplayWalFile(Env* env, const std::string& fname, MemTable* mem,
                     SequenceNumber first_sequence, WalReplayResult* result) {
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    *result = WalReplayResult();
    return s;
  }
  return ReplayWal(file.get(), mem, first_sequence, result);
}

}  // namespace lsm
