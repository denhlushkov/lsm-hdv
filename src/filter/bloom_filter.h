// Bloom filter (design doc, sections 2.1.3 and 4.2.1).
//
//   bits per key  m/n = -ln(p) / ln(2)^2      (p = 1% -> 9.59 -> 10)
//   hash count    k   = (m/n) * ln(2)         (10 bits/key -> 7)
//   probes        g_i(x) = h1(x) + i * h2(x)  (mod m), i in [0, k)
//
// h1 and h2 are the two 64-bit halves of MurmurHash3_x64_128
// (Kirsch-Mitzenmacher double hashing).
//
// Serialized filter: bit array (multiple of 8 bits) followed by one byte
// holding k, so a reader needs no configuration to evaluate a filter.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

class BloomFilterPolicy {
 public:
  static constexpr int kMaxProbes = 30;
  static constexpr size_t kMinBits = 64;

  explicit BloomFilterPolicy(int bits_per_key);

  int bits_per_key() const { return bits_per_key_; }
  int num_probes() const { return num_probes_; }

  // Appends a filter summarizing keys[0, n) to *dst.
  void CreateFilter(const std::string_view* keys, size_t n,
                    std::string* dst) const;
  void CreateFilter(const std::vector<std::string_view>& keys,
                    std::string* dst) const {
    CreateFilter(keys.data(), keys.size(), dst);
  }

  // False means the key is definitely absent. True means "maybe present".
  // Never returns false for a key that was passed to CreateFilter.
  static bool KeyMayMatch(std::string_view key, std::string_view filter);

  // k = round(bits_per_key * ln 2), clamped to [1, kMaxProbes].
  static int OptimalNumProbes(int bits_per_key);
  // m/n = -ln(p) / ln(2)^2.
  static double OptimalBitsPerKey(double false_positive_rate);
  // Theoretical false positive rate (1 - e^(-k / (m/n)))^k.
  static double ExpectedFalsePositiveRate(double bits_per_key, int num_probes);

 private:
  int bits_per_key_;
  int num_probes_;
};

// Accumulates the keys of one SSTable and produces its filter block.
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const BloomFilterPolicy* policy);

  // Consecutive duplicate keys are added only once.
  void AddKey(std::string_view key);
  size_t num_keys() const { return starts_.size(); }
  std::string Finish();

 private:
  const BloomFilterPolicy* policy_;
  std::string keys_;            // flattened key contents
  std::vector<size_t> starts_;  // offset of each key in keys_
};

}  // namespace lsm
