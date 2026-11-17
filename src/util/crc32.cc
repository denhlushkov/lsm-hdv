#include "util/crc32.h"

#include <array>

namespace lsm::crc32 {

namespace {

constexpr uint32_t kPolynomial = 0xEDB88320u;

// kTables[0] is the classic byte-wise table; kTables[1..3] allow processing
// four bytes per step ("slicing-by-4").
constexpr std::array<std::array<uint32_t, 256>, 4> MakeTables() {
  std::array<std::array<uint32_t, 256>, 4> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (kPolynomial ^ (c >> 1)) : (c >> 1);
    }
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t s = 1; s < 4; ++s) {
      t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    }
  }
  return t;
}

constexpr auto kTables = MakeTables();

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t c = init_crc ^ 0xFFFFFFFFu;
  while (n >= 4) {
    c ^= static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
    c = kTables[3][c & 0xFFu] ^ kTables[2][(c >> 8) & 0xFFu] ^
        kTables[1][(c >> 16) & 0xFFu] ^ kTables[0][c >> 24];
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    c = kTables[0][(c ^ *p) & 0xFFu] ^ (c >> 8);
    ++p;
    --n;
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace lsm::crc32
