// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), used by WAL records
// and SSTable block trailers (design doc, section 4.1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm::crc32 {

// Returns the CRC of concat(A, data[0, n-1]) where init_crc is the CRC of A.
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
inline uint32_t Value(std::string_view data) {
  return Extend(0, data.data(), data.size());
}

}  // namespace lsm::crc32
