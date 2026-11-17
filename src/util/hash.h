// MurmurHash3 (x64, 128-bit variant), used by the Bloom filter
// (design doc, section 4.2.1) and for cache sharding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm {

struct Hash128 {
  uint64_t h1;
  uint64_t h2;
};

Hash128 MurmurHash3_x64_128(const void* key, size_t len, uint32_t seed);

inline Hash128 MurmurHash3_x64_128(std::string_view s, uint32_t seed) {
  return MurmurHash3_x64_128(s.data(), s.size(), seed);
}

inline uint32_t Hash32(std::string_view s, uint32_t seed = 0) {
  return static_cast<uint32_t>(MurmurHash3_x64_128(s, seed).h1);
}

}  // namespace lsm
