// Endian-neutral binary encoding.
//  * Fixed-width integers are little-endian (design doc, section 4.1).
//  * Varints use 7 bits per byte, low groups first, high bit = "more".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

inline void EncodeFixed32(char* dst, uint32_t value) {
  auto* b = reinterpret_cast<uint8_t*>(dst);
  b[0] = static_cast<uint8_t>(value);
  b[1] = static_cast<uint8_t>(value >> 8);
  b[2] = static_cast<uint8_t>(value >> 16);
  b[3] = static_cast<uint8_t>(value >> 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  auto* b = reinterpret_cast<uint8_t*>(dst);
  for (int i = 0; i < 8; ++i) {
    b[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const auto* b = reinterpret_cast<const uint8_t*>(ptr);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const auto* b = reinterpret_cast<const uint8_t*>(ptr);
  uint64_t result = 0;
  for (int i = 7; i >= 0; --i) {
    result = (result << 8) | b[i];
  }
  return result;
}

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
// varint32 length followed by the bytes.
void PutLengthPrefixedSlice(std::string* dst, std::string_view value);

// Write a varint into dst; returns a pointer just past the last byte written.
// dst must have room for 5 (32-bit) or 10 (64-bit) bytes.
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// Number of bytes of the varint encoding of v.
int VarintLength(uint64_t v);

// Parse a varint from [p, limit). Returns a pointer past the parsed value,
// or nullptr if the input is truncated or the varint is malformed/overlong.
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

// Consume a value from the front of *input. Return false on malformed input
// (in which case *input is unspecified).
bool GetFixed32(std::string_view* input, uint32_t* value);
bool GetFixed64(std::string_view* input, uint64_t* value);
bool GetVarint32(std::string_view* input, uint32_t* value);
bool GetVarint64(std::string_view* input, uint64_t* value);
bool GetLengthPrefixedSlice(std::string_view* input, std::string_view* result);

}  // namespace lsm
