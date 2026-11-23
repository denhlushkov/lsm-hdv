# lsm-hdv

An embedded LSM-tree key-value storage engine written in C++20, with no
runtime dependencies.

The design document, in Ukrainian, is [docs/DESIGN.md](docs/DESIGN.md).
Section 9 of it records what is implemented so far and the format details
that were fixed during implementation.

## Status

Milestones **M0–M2** of the design are implemented:

| Module | Contents |
|---|---|
| `src/util/` | Status, varint/fixed coding, CRC32 (IEEE 802.3), MurmurHash3, arena |
| `src/env/` | File system abstraction: real files (`Env::Default()`) and in-memory (`NewMemEnv()`) |
| `src/memtable/` | Concurrent SkipList (p = 1/4, max height 16, bidirectional iterators); MemTable with tombstones and a flush threshold |
| `src/wal/` | Write-ahead log writer and reader; crash replay that stops at the first damaged record |
| `src/filter/` | Bloom filter: 10 bits/key and 7 probes using Kirsch–Mitzenmacher double hashing |
| `src/cache/` | Sharded LRU block cache (hash table + intrusive doubly linked list) |
| `src/sstable/` | Prefix-compressed data blocks with restart points; index, filter and footer; table builder and reader |
| `src/db/` | Internal key format, file names, MemTable → L0 SSTable flush |

Compaction and the public `StorageEngine` API belong to milestone M3 and are
not implemented yet.

## Building and testing

You need CMake ≥ 3.20 and a C++20 compiler (tested with GCC 16 / MinGW-w64
on Windows, and with GCC 11 and Clang 14 on Linux). The build fetches
GoogleTest and RapidCheck with git.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `LSM_BUILD_TESTS` | `ON` | Build the GoogleTest suite |
| `LSM_WITH_RAPIDCHECK` | `ON` | Build the property-based tests |
| `LSM_SANITIZE` | empty | Sanitizers to enable, e.g. `address,undefined` or `thread` (GCC/Clang) |
| `LSM_WERROR` | `OFF` | Treat warnings as errors |

The test suite has 100 GoogleTest cases in `lsm_tests` and 8 RapidCheck
properties in `lsm_property_tests`. Section 9.3 of the design document maps
the tests to the verification catalogue (U-01…U-05, P-01…P-03, FI-01, FI-02,
F-01/F-02 without libFuzzer).
