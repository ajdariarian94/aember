/**
 * @file property.h
 * @author Arian Ajdari
 * @brief Property-based tests on top of GoogleTest, without another
 *        framework: a property is checked on many generated cases, each
 *        from its own seed.
 *
 *   TEST(ResolverProperty, DependenciesComeFirst) {
 *     aember::test::ForAll(300, [](aember::test::Gen& g) {
 *       ...  EXPECT_...(...)  ...
 *     });
 *   }
 *
 * The seeds differ from run to run (more cases over time). The first
 * failing case stops the property and prints the seed that reproduces it
 * alone:
 *
 *   AEMBER_PROPERTY_SEED=<seed> AEMBER_PROPERTY_RUNS=1 <test>
 * --gtest_filter=...
 *
 * AEMBER_PROPERTY_RUNS=N runs every property N times (longer local runs).
 * There is no shrinking: generators keep sizes small, so a failing case is
 * small already.
 *
 * Why not RapidCheck (it is in the vcpkg tree): vcpkg.json is installed for
 * every target triplet, so a test-only dependency would be built for eight
 * architectures and invalidate every CI cache, for a few hundred lines we
 * can keep here.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace aember::test {

/// Random values for one case.
class Gen {
 public:
  explicit Gen(std::uint64_t seed) : rng_(seed) {}

  /// Uniform in [lo, hi].
  template <class T = int>
  T Int(T lo, T hi) {
    return std::uniform_int_distribution<T>{lo, hi}(rng_);
  }
  bool Chance(double p) { return std::bernoulli_distribution{p}(rng_); }
  /// A size in [0, max], small ones more often (edge cases live there).
  int Size(int max) {
    return Chance(0.5) ? Int(0, std::min(max, 4)) : Int(0, max);
  }
  /// Up to @p max random bytes.
  std::string Bytes(int max) {
    std::string s(static_cast<std::size_t>(Size(max)), '\0');
    for (auto& c : s) c = static_cast<char>(Int(0, 255));
    return s;
  }
  /// Characters from @p alphabet, length in [min, max].
  std::string Text(std::string_view alphabet, int min, int max) {
    std::string s(static_cast<std::size_t>(Int(min, max)), '\0');
    for (auto& c : s) {
      c = alphabet[Int<std::size_t>(0, alphabet.size() - 1)];
    }
    return s;
  }
  /// A copy: a reference into a temporary vector would dangle.
  template <class T>
  T Pick(const std::vector<T>& v) {
    return v[Int<std::size_t>(0, v.size() - 1)];
  }
  std::mt19937_64& Engine() { return rng_; }

 private:
  std::mt19937_64 rng_;
};

/// The run's base seed: AEMBER_PROPERTY_SEED, or a random one.
inline std::uint64_t BaseSeed() {
  static const std::uint64_t seed = []() -> std::uint64_t {
    if (const char* s = std::getenv("AEMBER_PROPERTY_SEED")) {
      return std::strtoull(s, nullptr, 10);
    }
    std::random_device rd;
    return (std::uint64_t{rd()} << 32) | rd();
  }();
  return seed;
}

/// Check @p property on @p runs cases (AEMBER_PROPERTY_RUNS overrides);
/// stop at the first failing case and name its seed.
template <class F>
void ForAll(int runs, F&& property) {
  if (const char* r = std::getenv("AEMBER_PROPERTY_RUNS")) {
    runs = std::atoi(r);
  }
  for (int i = 0; i < runs; ++i) {
    const std::uint64_t seed = BaseSeed() + static_cast<std::uint64_t>(i);
    Gen g{seed};
    {
      SCOPED_TRACE("reproduce with AEMBER_PROPERTY_SEED=" +
                   std::to_string(seed) + " AEMBER_PROPERTY_RUNS=1");
      property(g);
    }
    if (::testing::Test::HasFailure()) return;
  }
}

}  // namespace aember::test
