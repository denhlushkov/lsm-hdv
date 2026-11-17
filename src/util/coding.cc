#include "util/coding.h"

namespace lsm {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* EncodeVarint32(char* dst, uint32_t value) {
  return EncodeVarint64(dst, value);
}

char* EncodeVarint64(char* dst, uint64_t value) {
  constexpr uint64_t kMore = 0x80;
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  while (value >= kMore) {
    *(ptr++) = static_cast<uint8_t>(value | kMore);
    value >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(value);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string* dst, uint32_t value) {
  char buf[5];
  char* end = EncodeVarint32(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutVarint64(std::string* dst, uint64_t value) {
  char buf[10];
  char* end = EncodeVarint64(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutLengthPrefixedSlice(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    ++len;
  }
  return len;
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28; shift += 7) {
    if (p >= limit) {
      return nullptr;
    }
    const uint32_t byte = static_cast<uint8_t>(*p);
    ++p;
    if (shift == 28 && byte > 0x0F) {
      return nullptr;  // would overflow 32 bits
    }
    if (byte & 0x80) {
      result |= (byte & 0x7F) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63; shift += 7) {
    if (p >= limit) {
      return nullptr;
    }
    const uint64_t byte = static_cast<uint8_t>(*p);
    ++p;
    if (shift == 63 && byte > 0x01) {
      return nullptr;  // would overflow 64 bits
    }
    if (byte & 0x80) {
      result |= (byte & 0x7F) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetFixed32(std::string_view* input, uint32_t* value) {
  if (input->size() < sizeof(uint32_t)) {
    return false;
  }
  *value = DecodeFixed32(input->data());
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed64(std::string_view* input, uint64_t* value) {
  if (input->size() < sizeof(uint64_t)) {
    return false;
  }
  *value = DecodeFixed64(input->data());
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

bool GetVarint32(std::string_view* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(q - p));
  return true;
}

bool GetVarint64(std::string_view* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(q - p));
  return true;
}

bool GetLengthPrefixedSlice(std::string_view* input, std::string_view* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len) || input->size() < len) {
    return false;
  }
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return true;
}

}  // namespace lsm
