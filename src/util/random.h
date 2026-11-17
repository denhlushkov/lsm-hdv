// Small, fast, deterministic PRNG (xorshift64*). Not cryptographic.
#pragma once

#include <cstdint>

namespace lsm {

class Random {
 public:
  explicit Random(uint64_t seed)
      : state_(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL) {}

  uint64_t Next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 2685821657736338717ULL;
  }

  // Uniform in [0, n). REQUIRES: n > 0.
  uint64_t Uniform(uint64_t n) { return Next() % n; }

  // True with probability 1/n.
  bool OneIn(uint64_t n) { return Uniform(n) == 0; }

 private:
  uint64_t state_;
};

}  // namespace lsm
