// Tests for the Bloom filter (design doc: 4.2.1, U-02).
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "filter/bloom_filter.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsm {
namespace {

std::string IntKey(uint32_t i) {
  std::string s;
  PutFixed32(&s, i);
  return s;
}

std::string BuildFilter(const BloomFilterPolicy& policy,
                        const std::vector<std::string>& keys) {
  std::vector<std::string_view> views(keys.begin(), keys.end());
  std::string filter;
  policy.CreateFilter(views, &filter);
  return filter;
}

TEST(BloomFilterTest, ParametersFollowTheFormulas) {
  // m/n = -ln(0.01) / ln(2)^2 ~ 9.59 -> 10 bits per key.
  EXPECT_NEAR(BloomFilterPolicy::OptimalBitsPerKey(0.01), 9.585, 0.01);
  // k = 10 * ln 2 ~ 6.93 -> 7 probes.
  EXPECT_EQ(BloomFilterPolicy::OptimalNumProbes(10), 7);
  EXPECT_EQ(BloomFilterPolicy(10).num_probes(), 7);
  EXPECT_EQ(BloomFilterPolicy::OptimalNumProbes(1), 1);
  EXPECT_EQ(BloomFilterPolicy::OptimalNumProbes(1000), BloomFilterPolicy::kMaxProbes);
  EXPECT_LT(BloomFilterPolicy::ExpectedFalsePositiveRate(10, 7), 0.01);
}

TEST(BloomFilterTest, EmptyFilterMatchesNothing) {
  BloomFilterPolicy policy(10);
  const std::string filter = BuildFilter(policy, {});
  EXPECT_EQ(filter.size(), BloomFilterPolicy::kMinBits / 8 + 1);
  EXPECT_FALSE(BloomFilterPolicy::KeyMayMatch("hello", filter));
  EXPECT_FALSE(BloomFilterPolicy::KeyMayMatch("", filter));
  EXPECT_FALSE(BloomFilterPolicy::KeyMayMatch("x", ""));  // no filter bits
}

TEST(BloomFilterTest, SmallFilter) {
  BloomFilterPolicy policy(10);
  const std::string filter = BuildFilter(policy, {"hello", "world"});
  EXPECT_TRUE(BloomFilterPolicy::KeyMayMatch("hello", filter));
  EXPECT_TRUE(BloomFilterPolicy::KeyMayMatch("world", filter));
  EXPECT_FALSE(BloomFilterPolicy::KeyMayMatch("x", filter));
  EXPECT_FALSE(BloomFilterPolicy::KeyMayMatch("foo", filter));
}

// U-02: 0% false negatives and an empirical false positive rate <= 1.2% at
// 10 bits/key, for a range of filter sizes.
TEST(BloomFilterTest, NoFalseNegativesAndLowFalsePositiveRate) {
  BloomFilterPolicy policy(10);
  for (uint32_t n : {1u, 10u, 100u, 1000u, 10000u, 100000u}) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      keys.push_back(IntKey(i));
    }
    const std::string filter = BuildFilter(policy, keys);
    EXPECT_LE(filter.size(), (n * 10 + 7) / 8 + 40) << "n=" << n;

    for (const auto& key : keys) {
      ASSERT_TRUE(BloomFilterPolicy::KeyMayMatch(key, filter))
          << "false negative, n=" << n;
    }

    if (n >= 1000) {
      int false_positives = 0;
      constexpr int kProbes = 100000;
      for (uint32_t i = 0; i < kProbes; ++i) {
        if (BloomFilterPolicy::KeyMayMatch(IntKey(i + 1000000000u), filter)) {
          ++false_positives;
        }
      }
      const double rate = static_cast<double>(false_positives) / kProbes;
      EXPECT_LE(rate, 0.012) << "n=" << n;
    }
  }
}

TEST(BloomFilterTest, FewerBitsPerKeyMeansMoreFalsePositives) {
  std::vector<std::string> keys;
  for (uint32_t i = 0; i < 10000; ++i) {
    keys.push_back("key" + std::to_string(i));
  }
  auto fp_rate = [&](int bits) {
    BloomFilterPolicy policy(bits);
    const std::string filter = BuildFilter(policy, keys);
    int fp = 0;
    for (uint32_t i = 0; i < 20000; ++i) {
      fp += BloomFilterPolicy::KeyMayMatch("absent" + std::to_string(i), filter);
    }
    return fp / 20000.0;
  };
  const double r4 = fp_rate(4);
  const double r10 = fp_rate(10);
  const double r20 = fp_rate(20);
  EXPECT_GT(r4, r10);
  EXPECT_GT(r10, r20);
  EXPECT_NEAR(r4, BloomFilterPolicy::ExpectedFalsePositiveRate(4, 3), 0.05);
}

TEST(BloomFilterTest, UnknownProbeCountIsTreatedAsMatch) {
  BloomFilterPolicy policy(10);
  std::string filter = BuildFilter(policy, {"a"});
  filter.back() = static_cast<char>(BloomFilterPolicy::kMaxProbes + 1);
  EXPECT_TRUE(BloomFilterPolicy::KeyMayMatch("anything", filter));
}

TEST(BloomFilterTest, FilterBlockBuilderDeduplicatesConsecutiveKeys) {
  BloomFilterPolicy policy(10);
  FilterBlockBuilder builder(&policy);
  builder.AddKey("a");
  builder.AddKey("a");
  builder.AddKey("b");
  builder.AddKey("b");
  builder.AddKey("c");
  EXPECT_EQ(builder.num_keys(), 3u);
  const std::string filter = builder.Finish();
  EXPECT_TRUE(BloomFilterPolicy::KeyMayMatch("a", filter));
  EXPECT_TRUE(BloomFilterPolicy::KeyMayMatch("b", filter));
  EXPECT_TRUE(BloomFilterPolicy::KeyMayMatch("c", filter));
  EXPECT_EQ(builder.num_keys(), 0u);  // reset after Finish
}

}  // namespace
}  // namespace lsm
