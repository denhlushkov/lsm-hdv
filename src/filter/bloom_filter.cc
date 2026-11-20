#include "filter/bloom_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "util/hash.h"

namespace lsm {

namespace {

constexpr uint32_t kBloomSeed = 0xbc9f1d34;
constexpr double kLn2 = 0.69314718055994530942;

// Bit index of probe i: (h1 + i * h2) mod bits.
inline uint64_t ProbeBit(uint64_t h1, uint64_t h2, int i, uint64_t bits) {
  return (h1 + static_cast<uint64_t>(i) * h2) % bits;
}

}  // namespace

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key)
    : bits_per_key_(std::max(bits_per_key, 1)),
      num_probes_(OptimalNumProbes(bits_per_key)) {}

int BloomFilterPolicy::OptimalNumProbes(int bits_per_key) {
  const int k = static_cast<int>(std::lround(bits_per_key * kLn2));
  return std::clamp(k, 1, kMaxProbes);
}

double BloomFilterPolicy::OptimalBitsPerKey(double false_positive_rate) {
  return -std::log(false_positive_rate) / (kLn2 * kLn2);
}

double BloomFilterPolicy::ExpectedFalsePositiveRate(double bits_per_key,
                                                    int num_probes) {
  return std::pow(1.0 - std::exp(-num_probes / bits_per_key), num_probes);
}

void BloomFilterPolicy::CreateFilter(const std::string_view* keys, size_t n,
                                     std::string* dst) const {
  size_t bits = n * static_cast<size_t>(bits_per_key_);
  // Very small filters have a high false positive rate; enforce a minimum.
  bits = std::max(bits, kMinBits);
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const size_t init_size = dst->size();
  dst->resize(init_size + bytes, 0);
  dst->push_back(static_cast<char>(num_probes_));
  auto* array = reinterpret_cast<uint8_t*>(dst->data() + init_size);
  for (size_t i = 0; i < n; ++i) {
    const Hash128 h = MurmurHash3_x64_128(keys[i], kBloomSeed);
    for (int j = 0; j < num_probes_; ++j) {
      const uint64_t bit = ProbeBit(h.h1, h.h2, j, bits);
      array[bit / 8] = static_cast<uint8_t>(array[bit / 8] | (1u << (bit % 8)));
    }
  }
}

bool BloomFilterPolicy::KeyMayMatch(std::string_view key,
                                    std::string_view filter) {
  const size_t len = filter.size();
  if (len < 2) {
    return false;  // no bits: an empty set
  }
  const auto* array = reinterpret_cast<const uint8_t*>(filter.data());
  const uint64_t bits = static_cast<uint64_t>(len - 1) * 8;
  const int k = array[len - 1];
  if (k == 0 || k > kMaxProbes) {
    // Unknown encoding (e.g. a future format); err on the safe side.
    return true;
  }
  const Hash128 h = MurmurHash3_x64_128(key, kBloomSeed);
  for (int j = 0; j < k; ++j) {
    const uint64_t bit = ProbeBit(h.h1, h.h2, j, bits);
    if ((array[bit / 8] & (1u << (bit % 8))) == 0) {
      return false;
    }
  }
  return true;
}

FilterBlockBuilder::FilterBlockBuilder(const BloomFilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::AddKey(std::string_view key) {
  if (!starts_.empty()) {
    const std::string_view last(keys_.data() + starts_.back(),
                                keys_.size() - starts_.back());
    if (last == key) {
      return;
    }
  }
  starts_.push_back(keys_.size());
  keys_.append(key.data(), key.size());
}

std::string FilterBlockBuilder::Finish() {
  std::vector<std::string_view> keys;
  keys.reserve(starts_.size());
  for (size_t i = 0; i < starts_.size(); ++i) {
    const size_t end = i + 1 < starts_.size() ? starts_[i + 1] : keys_.size();
    keys.emplace_back(keys_.data() + starts_[i], end - starts_[i]);
  }
  std::string result;
  policy_->CreateFilter(keys, &result);
  keys_.clear();
  starts_.clear();
  return result;
}

}  // namespace lsm
