// Flush: writes the contents of an (immutable) MemTable into a new Level-0
// SSTable with its index block and Bloom filter (design doc, section 3.1).
#pragma once

#include <cstdint>
#include <string>

#include "db/dbformat.h"
#include "lsm/env.h"
#include "lsm/iterator.h"
#include "lsm/options.h"
#include "lsm/status.h"

namespace lsm {

class MemTable;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;  // 0 if no file was produced (empty input)
  uint64_t num_entries = 0;
  std::string smallest;  // smallest internal key in the file
  std::string largest;   // largest internal key in the file
};

// Writes every entry of iter (internal keys in increasing order) to the
// table file `number` in dbname. The table is written to a temporary file,
// synced, verified by re-opening it, and only then renamed to its final
// name, so a crash or an error (e.g. ENOSPC) never leaves a half-written
// .sst behind. On failure the caller's data (the MemTable) is untouched and
// the returned status describes the error.
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  const InternalKeyComparator& icmp, Iterator* iter,
                  uint64_t number, FileMetaData* meta);

// Convenience wrapper: flushes an immutable MemTable.
Status FlushMemTable(const std::string& dbname, Env* env,
                     const Options& options, const MemTable& mem,
                     uint64_t number, FileMetaData* meta);

}  // namespace lsm
