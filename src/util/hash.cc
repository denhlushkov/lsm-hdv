// Port of Austin Appleby's public-domain MurmurHash3_x64_128. Block reads use
// explicit little-endian decoding so the result is platform independent.
#include "util/hash.h"

#include "util/coding.h"

namespace lsm {

namespace {

inline uint64_t Rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

inline uint64_t Fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

constexpr uint64_t kC1 = 0x87c37b91114253d5ULL;
constexpr uint64_t kC2 = 0x4cf5ad432745937fULL;

}  // namespace

Hash128 MurmurHash3_x64_128(const void* key, size_t len, uint32_t seed) {
  const auto* data = static_cast<const uint8_t*>(key);
  const size_t nblocks = len / 16;
  uint64_t h1 = seed;
  uint64_t h2 = seed;

  for (size_t i = 0; i < nblocks; ++i) {
    const char* block = reinterpret_cast<const char*>(data + i * 16);
    uint64_t k1 = DecodeFixed64(block);
    uint64_t k2 = DecodeFixed64(block + 8);

    k1 *= kC1;
    k1 = Rotl64(k1, 31);
    k1 *= kC2;
    h1 ^= k1;
    h1 = Rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;

    k2 *= kC2;
    k2 = Rotl64(k2, 33);
    k2 *= kC1;
    h2 ^= k2;
    h2 = Rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  const uint8_t* tail = data + nblocks * 16;
  uint64_t k1 = 0;
  uint64_t k2 = 0;
  switch (len & 15) {
    case 15: k2 ^= static_cast<uint64_t>(tail[14]) << 48; [[fallthrough]];
    case 14: k2 ^= static_cast<uint64_t>(tail[13]) << 40; [[fallthrough]];
    case 13: k2 ^= static_cast<uint64_t>(tail[12]) << 32; [[fallthrough]];
    case 12: k2 ^= static_cast<uint64_t>(tail[11]) << 24; [[fallthrough]];
    case 11: k2 ^= static_cast<uint64_t>(tail[10]) << 16; [[fallthrough]];
    case 10: k2 ^= static_cast<uint64_t>(tail[9]) << 8; [[fallthrough]];
    case 9:
      k2 ^= static_cast<uint64_t>(tail[8]);
      k2 *= kC2;
      k2 = Rotl64(k2, 33);
      k2 *= kC1;
      h2 ^= k2;
      [[fallthrough]];
    case 8: k1 ^= static_cast<uint64_t>(tail[7]) << 56; [[fallthrough]];
    case 7: k1 ^= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
    case 6: k1 ^= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
    case 5: k1 ^= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
    case 4: k1 ^= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
    case 3: k1 ^= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
    case 2: k1 ^= static_cast<uint64_t>(tail[1]) << 8; [[fallthrough]];
    case 1:
      k1 ^= static_cast<uint64_t>(tail[0]);
      k1 *= kC1;
      k1 = Rotl64(k1, 31);
      k1 *= kC2;
      h1 ^= k1;
      break;
    default:
      break;
  }

  h1 ^= static_cast<uint64_t>(len);
  h2 ^= static_cast<uint64_t>(len);
  h1 += h2;
  h2 += h1;
  h1 = Fmix64(h1);
  h2 = Fmix64(h2);
  h1 += h2;
  h2 += h1;
  return Hash128{h1, h2};
}

}  // namespace lsm
