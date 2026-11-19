// Crash recovery: rebuilds a MemTable from a WAL segment (design doc, 2.1.1).
#pragma once

#include <cstdint>
#include <string>

#include "db/dbformat.h"
#include "lsm/env.h"
#include "lsm/status.h"

namespace lsm {

class MemTable;

struct WalReplayResult {
  uint64_t records_applied = 0;
  // Offset just past the last applied record. A recovering engine can
  // truncate the segment to this length before appending to it again.
  uint64_t valid_bytes = 0;
  // True if the log ended with a partially written record.
  bool tail_truncated = false;
  // True if replay stopped at a record with a bad checksum/format.
  bool stopped_at_corruption = false;
  // Sequence number assigned to the last applied record
  // (first_sequence - 1 if nothing was applied).
  SequenceNumber last_sequence = 0;
};

// Applies every intact record of the log to *mem, assigning consecutive
// sequence numbers starting at first_sequence. Returns OK when the log ended
// cleanly or with a torn tail, Corruption when a damaged record was found
// (all records before it are still applied), or an I/O error.
Status ReplayWal(SequentialFile* file, MemTable* mem,
                 SequenceNumber first_sequence, WalReplayResult* result);

Status ReplayWalFile(Env* env, const std::string& fname, MemTable* mem,
                     SequenceNumber first_sequence, WalReplayResult* result);

}  // namespace lsm
