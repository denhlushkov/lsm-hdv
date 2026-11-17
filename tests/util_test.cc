// Tests for util/: binary coding, CRC32, MurmurHash3, Status, comparators,
// internal keys and file names.
#include <cstring>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "db/dbformat.h"
#include "db/filename.h"
#include "lsm/comparator.h"
#include "lsm/status.h"
#include "test_util.h"
#include "util/coding.h"
#include "util/crc32.h"
#include "util/hash.h"

namespace lsm {
namespace {

// ---------------------------------------------------------------- coding

TEST(CodingTest, Fixed32And64AreLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201u);
  PutFixed64(&s, 0x0807060504030201ull);
  ASSERT_EQ(s.size(), 12u);
  const unsigned char expected[] = {1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(std::memcmp(s.data(), expected, sizeof(expected)), 0);
  EXPECT_EQ(DecodeFixed32(s.data()), 0x04030201u);
  EXPECT_EQ(DecodeFixed64(s.data() + 4), 0x0807060504030201ull);
}

TEST(CodingTest, Varint32RoundTripAtBoundaries) {
  std::vector<uint32_t> values;
  for (uint32_t shift = 0; shift < 32; ++shift) {
    const uint32_t v = 1u << shift;
    values.push_back(v - 1);
    values.push_back(v);
    values.push_back(v + 1);
  }
  values.push_back(std::numeric_limits<uint32_t>::max());

  std::string s;
  for (uint32_t v : values) {
    PutVarint32(&s, v);
  }
  std::string_view input(s);
  for (uint32_t expected : values) {
    uint32_t actual = 0;
    const size_t before = input.size();
    ASSERT_TRUE(GetVarint32(&input, &actual));
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(before - input.size(), static_cast<size_t>(VarintLength(expected)));
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, Varint64RoundTripAtBoundaries) {
  std::vector<uint64_t> values = {0, 100, std::numeric_limits<uint64_t>::max(),
                                  std::numeric_limits<uint64_t>::max() - 1};
  for (uint32_t k = 0; k < 64; ++k) {
    const uint64_t power = 1ull << k;
    values.push_back(power);
    values.push_back(power - 1);
    values.push_back(power + 1);
  }
  std::string s;
  for (uint64_t v : values) {
    PutVarint64(&s, v);
  }
  std::string_view input(s);
  for (uint64_t expected : values) {
    uint64_t actual = 0;
    ASSERT_TRUE(GetVarint64(&input, &actual));
    EXPECT_EQ(actual, expected);
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, TruncatedVarintIsRejected) {
  std::string s;
  PutVarint32(&s, std::numeric_limits<uint32_t>::max());
  for (size_t len = 0; len < s.size(); ++len) {
    uint32_t value = 0;
    EXPECT_EQ(GetVarint32Ptr(s.data(), s.data() + len, &value), nullptr)
        << "prefix length " << len;
  }
}

TEST(CodingTest, OverlongVarintIsRejected) {
  // Five continuation bytes: more than 32 bits.
  const std::string too_long32("\xff\xff\xff\xff\xff\x01", 6);
  uint32_t v32 = 0;
  EXPECT_EQ(GetVarint32Ptr(too_long32.data(),
                           too_long32.data() + too_long32.size(), &v32),
            nullptr);
  // Fifth byte with bits above 2^32.
  const std::string overflow32("\xff\xff\xff\xff\x1f", 5);
  EXPECT_EQ(GetVarint32Ptr(overflow32.data(),
                           overflow32.data() + overflow32.size(), &v32),
            nullptr);
  // Eleven bytes for a 64-bit varint.
  const std::string too_long64(11, '\xff');
  uint64_t v64 = 0;
  EXPECT_EQ(GetVarint64Ptr(too_long64.data(),
                           too_long64.data() + too_long64.size(), &v64),
            nullptr);
}

TEST(CodingTest, LengthPrefixedSlices) {
  std::string s;
  PutLengthPrefixedSlice(&s, "");
  PutLengthPrefixedSlice(&s, "foo");
  PutLengthPrefixedSlice(&s, std::string(300, 'x'));
  std::string_view input(s);
  std::string_view v;
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ(v, "");
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ(v, "foo");
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ(v, std::string(300, 'x'));
  EXPECT_FALSE(GetLengthPrefixedSlice(&input, &v));  // exhausted

  // Length says 5 but only 2 bytes follow.
  std::string bad;
  PutVarint32(&bad, 5);
  bad += "ab";
  std::string_view bad_input(bad);
  EXPECT_FALSE(GetLengthPrefixedSlice(&bad_input, &v));
}

// ---------------------------------------------------------------- CRC32

TEST(Crc32Test, StandardCheckValues) {
  // Check value of CRC-32/ISO-HDLC (IEEE 802.3).
  EXPECT_EQ(crc32::Value("123456789"), 0xCBF43926u);
  EXPECT_EQ(crc32::Value(""), 0u);
  EXPECT_EQ(crc32::Value("a"), 0xE8B7BE43u);
  EXPECT_EQ(crc32::Value("The quick brown fox jumps over the lazy dog"),
            0x414FA339u);
}

TEST(Crc32Test, ExtendIsConcatenation) {
  Random rnd(301);
  const std::string data = test::RandomBytes(&rnd, 1000);
  const uint32_t whole = crc32::Value(data);
  for (size_t split : {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{5},
                       size_t{500}, size_t{999}, size_t{1000}}) {
    const uint32_t first = crc32::Value(data.data(), split);
    EXPECT_EQ(crc32::Extend(first, data.data() + split, data.size() - split),
              whole)
        << "split at " << split;
  }
}

TEST(Crc32Test, DetectsEverySingleBitFlip) {
  std::string data = "lsm-hdv write-ahead log payload";
  const uint32_t original = crc32::Value(data);
  for (size_t i = 0; i < data.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      data[i] = static_cast<char>(data[i] ^ (1 << bit));
      EXPECT_NE(crc32::Value(data), original);
      data[i] = static_cast<char>(data[i] ^ (1 << bit));
    }
  }
}

// ---------------------------------------------------------- MurmurHash3

TEST(MurmurHashTest, EmptyInputWithZeroSeedIsZero) {
  const Hash128 h = MurmurHash3_x64_128("", 0);
  EXPECT_EQ(h.h1, 0u);
  EXPECT_EQ(h.h2, 0u);
}

TEST(MurmurHashTest, SMHasherVerificationValue) {
  // Verification procedure from SMHasher: hash keys {0}, {0,1}, ...,
  // {0..254} with seed 256 - len, then hash the concatenated results.
  uint8_t key[256];
  uint8_t hashes[256 * 16];
  for (int i = 0; i < 256; ++i) {
    key[i] = static_cast<uint8_t>(i);
    const Hash128 h =
        MurmurHash3_x64_128(key, static_cast<size_t>(i), 256u - i);
    EncodeFixed64(reinterpret_cast<char*>(hashes + i * 16), h.h1);
    EncodeFixed64(reinterpret_cast<char*>(hashes + i * 16 + 8), h.h2);
  }
  const Hash128 final_hash = MurmurHash3_x64_128(hashes, sizeof(hashes), 0);
  EXPECT_EQ(static_cast<uint32_t>(final_hash.h1), 0x6384BA69u);
}

TEST(MurmurHashTest, SeedAndInputChangeTheHash) {
  const Hash128 a = MurmurHash3_x64_128("hello", 0);
  const Hash128 b = MurmurHash3_x64_128("hello", 1);
  const Hash128 c = MurmurHash3_x64_128("hellp", 0);
  EXPECT_TRUE(a.h1 != b.h1 || a.h2 != b.h2);
  EXPECT_TRUE(a.h1 != c.h1 || a.h2 != c.h2);
  const Hash128 again = MurmurHash3_x64_128("hello", 0);
  EXPECT_EQ(a.h1, again.h1);
  EXPECT_EQ(a.h2, again.h2);
}

// ---------------------------------------------------------------- Status

TEST(StatusTest, CodesAndMessages) {
  EXPECT_TRUE(Status().ok());
  EXPECT_EQ(Status::OK().to_string(), "OK");

  const Status nf = Status::NotFound("missing");
  EXPECT_TRUE(nf.is_not_found());
  EXPECT_FALSE(nf.ok());
  EXPECT_EQ(nf.to_string(), "NotFound: missing");

  EXPECT_TRUE(Status::Corruption("x").is_corruption());
  EXPECT_TRUE(Status::IOError().is_io_error());
  EXPECT_EQ(Status::IOError().to_string(), "IO error");
  EXPECT_TRUE(Status::InvalidArgument("bad").is_invalid_argument());
  EXPECT_EQ(Status(StatusCode::kCorruption, "crc").to_string(),
            "Corruption: crc");
}

// ------------------------------------------------------ comparators/keys

TEST(ComparatorTest, BytewiseIsUnsignedLexicographic) {
  const Comparator* cmp = BytewiseComparator();
  EXPECT_LT(cmp->Compare("a", "b"), 0);
  EXPECT_GT(cmp->Compare("b", "a"), 0);
  EXPECT_EQ(cmp->Compare("abc", "abc"), 0);
  EXPECT_LT(cmp->Compare("ab", "abc"), 0);  // prefix sorts first
  EXPECT_LT(cmp->Compare("", "a"), 0);
  // 0xff must sort after 0x01 (unsigned comparison).
  EXPECT_LT(cmp->Compare(std::string(1, '\x01'), std::string(1, '\xff')), 0);
}

TEST(InternalKeyTest, EncodeParseRoundTrip) {
  const std::string ikey = MakeInternalKey("user", 12345, kTypeDeletion);
  ASSERT_EQ(ikey.size(), 4u + 8u);
  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(ikey, &parsed));
  EXPECT_EQ(parsed.user_key, "user");
  EXPECT_EQ(parsed.sequence, 12345u);
  EXPECT_EQ(parsed.type, kTypeDeletion);
  EXPECT_EQ(ExtractUserKey(ikey), "user");

  EXPECT_FALSE(ParseInternalKey("short", &parsed));
  std::string bad_type = MakeInternalKey("k", 1, kTypeValue);
  bad_type[bad_type.size() - 8] = 0x7f;  // type byte
  EXPECT_FALSE(ParseInternalKey(bad_type, &parsed));
}

TEST(InternalKeyTest, OrderIsUserKeyAscSequenceDesc) {
  const InternalKeyComparator icmp(BytewiseComparator());
  const std::string a5 = MakeInternalKey("a", 5, kTypeValue);
  const std::string a7 = MakeInternalKey("a", 7, kTypeValue);
  const std::string b1 = MakeInternalKey("b", 1, kTypeValue);
  EXPECT_LT(icmp.Compare(a7, a5), 0);  // newer first
  EXPECT_LT(icmp.Compare(a5, b1), 0);
  EXPECT_LT(icmp.Compare(a7, b1), 0);
  EXPECT_EQ(icmp.Compare(a5, a5), 0);
  // A lookup key at sequence 6 sorts after a7 and before a5.
  const LookupKey lk("a", 6);
  EXPECT_GT(icmp.Compare(lk.internal_key(), a7), 0);
  EXPECT_LT(icmp.Compare(lk.internal_key(), a5), 0);
  EXPECT_EQ(lk.user_key(), "a");
}

TEST(FileNameTest, BuildAndParse) {
  EXPECT_EQ(WalFileName("db", 7), "db/000007.log");
  EXPECT_EQ(TableFileName("db", 123456789), "db/123456789.sst");
  uint64_t number = 0;
  FileType type = FileType::kTempFile;
  ASSERT_TRUE(ParseFileName("000007.log", &number, &type));
  EXPECT_EQ(number, 7u);
  EXPECT_EQ(type, FileType::kWalFile);
  ASSERT_TRUE(ParseFileName("42.sst", &number, &type));
  EXPECT_EQ(number, 42u);
  EXPECT_EQ(type, FileType::kTableFile);
  EXPECT_FALSE(ParseFileName("LOCK", &number, &type));
  EXPECT_FALSE(ParseFileName("12.txt", &number, &type));
  EXPECT_FALSE(ParseFileName(".log", &number, &type));
  EXPECT_FALSE(ParseFileName("99999999999999999999999.log", &number, &type));
}

}  // namespace
}  // namespace lsm
