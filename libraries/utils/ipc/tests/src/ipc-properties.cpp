/**
 * @file ipc-properties.cpp
 * @author Arian Ajdari
 * @brief Property tests for the PID 1 <-> daemon channel: any JSON object
 *        up to kMaxMessage arrives whole and equal, in order; bigger ones
 *        are refused by Send; anything but an object is no message.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/channel.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <aember-test/property.h>

namespace {

using aember::test::Gen;
using aember::utils::ipc::Channel;
using aember::utils::ipc::kMaxMessage;
using namespace std::chrono_literals;

std::string AnyText(Gen& g, int max = 12) {
  static const std::vector<std::string> pieces{"a",
                                               "z",
                                               "0",
                                               " ",
                                               "\"",
                                               "\\",
                                               "\n",
                                               "\x01",
                                               "\xc3\xa9",
                                               "\xe2\x82\xac",
                                               "\xf0\x9f\x94\xa5"};
  std::string s;
  for (int i = g.Size(max); i > 0; --i) s += g.Pick(pieces);
  return s;
}

/// Any JSON value (no NaN: JSON has none, dump() writes null).
nlohmann::json AnyValue(Gen& g, int depth = 0) {
  switch (g.Int(0, depth < 3 ? 7 : 5)) {
    case 0:
      return nullptr;
    case 1:
      return g.Chance(0.5);
    case 2:
      return g.Int<std::int64_t>(INT64_MIN, INT64_MAX);
    case 3:
      return g.Int<std::uint64_t>(0, UINT64_MAX);
    case 4: {
      const double d =
          std::uniform_real_distribution<double>{-1e300, 1e300}(g.Engine());
      return g.Chance(0.3) ? d / 1e290 : d;
    }
    case 5:
      return AnyText(g);
    case 6: {
      auto a = nlohmann::json::array();
      for (int i = g.Size(4); i > 0; --i) a.push_back(AnyValue(g, depth + 1));
      return a;
    }
    default: {
      auto o = nlohmann::json::object();
      for (int i = g.Size(4); i > 0; --i)
        o[AnyText(g, 6)] = AnyValue(g, depth + 1);
      return o;
    }
  }
}

nlohmann::json AnyMessage(Gen& g) {
  auto m = nlohmann::json::object();
  m["type"] = AnyText(g, 6);
  for (int i = g.Size(4); i > 0; --i) m[AnyText(g, 6)] = AnyValue(g);
  return m;
}

TEST(ChannelProperty, ObjectsArriveWholeEqualAndInOrder) {
  aember::test::ForAll(200, [](Gen& g) {
    auto pair = Channel::Pair();
    ASSERT_TRUE(pair);
    auto& [a, b] = *pair;
    std::vector<nlohmann::json> sent;
    for (int i = g.Int(1, 8); i > 0; --i) {
      sent.push_back(AnyMessage(g));
      ASSERT_TRUE(a.Send(sent.back())) << sent.back().dump();
    }
    for (const auto& want : sent) {
      const auto got = b.Receive(1s);
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ(*got, want);
    }
    EXPECT_FALSE(b.Receive(0ms).has_value());  // nothing more
    EXPECT_FALSE(b.Closed());
  });
}

TEST(ChannelProperty, TooBigIsRefusedUpToTheLimitArrives) {
  aember::test::ForAll(50, [](Gen& g) {
    auto pair = Channel::Pair();
    ASSERT_TRUE(pair);
    auto& [a, b] = *pair;
    // {"pad":"...","type":"x"}: 21 bytes around the padding.
    const std::size_t size = g.Chance(0.5)
                                 ? kMaxMessage - g.Int<std::size_t>(0, 3)
                                 : kMaxMessage + g.Int<std::size_t>(1, 3);
    const nlohmann::json m{{"type", "x"}, {"pad", std::string(size - 21, 'p')}};
    ASSERT_EQ(m.dump().size(), size);
    EXPECT_EQ(a.Send(m), size <= kMaxMessage);
    if (size <= kMaxMessage) {
      const auto got = b.Receive(1s);
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ(*got, m);
    } else {
      EXPECT_FALSE(b.Receive(0ms).has_value());
    }
  });
}

TEST(ChannelProperty, AnythingButAnObjectIsNoMessage) {
  aember::test::ForAll(200, [](Gen& g) {
    auto pair = Channel::Pair();
    ASSERT_TRUE(pair);
    auto& [a, b] = *pair;
    auto v = AnyValue(g);
    if (v.is_object()) v = nlohmann::json::array({v});
    ASSERT_TRUE(a.Send(v));
    EXPECT_FALSE(b.Receive(1s).has_value());
    EXPECT_FALSE(b.Closed());  // still usable
    ASSERT_TRUE(a.Send(aember::utils::ipc::Ready()));
    const auto ready = b.Receive(1s);
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(aember::utils::ipc::TypeOf(*ready), "ready");
  });
}

}  // namespace
