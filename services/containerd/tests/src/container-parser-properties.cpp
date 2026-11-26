/**
 * @file container-parser-properties.cpp
 * @author Arian Ajdari
 * @brief Property tests for a container's config.json (ParseContainerConfig):
 * what is written comes back, a field of the wrong type is an error (not a
 *        crash or a default), numbers too big for an unsigned are refused.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/containerd/container-parser.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <climits>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <aember-test/property.h>

namespace {

using aember::containerd::ContainerConfig;
using aember::containerd::ParseContainerConfig;
using aember::containerd::RestartPolicy;
using aember::test::Gen;

std::string AnyText(Gen& g) {
  static const std::vector<std::string> pieces{
      "a", "b", "1", "-", "/", "\"", "\\", "\n", "\x01", " ", "\xc3\xa9"};
  std::string s;
  for (int i = g.Size(10); i > 0; --i) s += g.Pick(pieces);
  return s;
}

unsigned AnyUnsigned(Gen& g) {
  return g.Chance(0.2) ? g.Pick(std::vector<unsigned>{0, 1, UINT_MAX})
                       : g.Int<unsigned>(0, 100000);
}

/// A container's config.json (as in its package) and what it means.
std::pair<ContainerConfig, nlohmann::json> RandomContainer(Gen& g) {
  ContainerConfig c;
  c.name = "c" + g.Text("abcdefghijklmnopqrstuvwxyz0123456789._-", 0, 20);
  if (c.name.ends_with(".new") || c.name.ends_with(".prev")) c.name += "x";
  c.squashfs = "/var/lib/aember/" + c.name + "/image.squashfs";
  c.rootfs = "/var/lib/aember/containers/" + c.name;
  c.config_path = "/var/lib/aember/" + c.name + "/lxc.container.conf";
  for (int i = g.Int(1, 4); i > 0; --i) c.args.push_back(AnyText(g));
  nlohmann::json j{{"name", c.name}, {"command", c.args}};
  if (g.Chance(0.5)) j["init"] = c.init = g.Chance(0.5);
  if (g.Chance(0.5)) {
    const auto s = AnyUnsigned(g);
    j["stop_timeout"] = s;
    c.stop_timeout = std::chrono::seconds{s};
  }
  if (g.Chance(0.5)) {
    static const std::vector<std::pair<std::string, RestartPolicy>> policies{
        {"no", RestartPolicy::kNo},
        {"never", RestartPolicy::kNo},
        {"on-failure", RestartPolicy::kOnFailure},
        {"always", RestartPolicy::kAlways}};
    const auto [text, policy] = g.Pick(policies);
    j["restart_policy"] = text;
    c.restart_policy = policy;
  }
  if (g.Chance(0.5))
    j["max_restart_attempts"] = c.max_restart_attempts = AnyUnsigned(g);
  if (g.Chance(0.5)) {
    const auto s = AnyUnsigned(g);
    j["restart_delay"] = s;
    c.restart_delay = std::chrono::seconds{s};
  }
  return {c, j};
}

/// Parsed from its text, as containerd reads it from the disk.
bool Parses(const nlohmann::json& j, int indent = -1) {
  return ParseContainerConfig(nlohmann::json::parse(j.dump(indent)))
      .has_value();
}

TEST(ContainerConfigProperty, WhatIsWrittenComesBack) {
  aember::test::ForAll(300, [](Gen& g) {
    const auto [want, j] = RandomContainer(g);
    const auto got =
        ParseContainerConfig(nlohmann::json::parse(j.dump(g.Int(-1, 2))));
    ASSERT_TRUE(got) << got.error() << ": " << j.dump();
    EXPECT_EQ(got->name, want.name);
    EXPECT_EQ(got->squashfs, want.squashfs);
    EXPECT_EQ(got->rootfs, want.rootfs);
    EXPECT_EQ(got->config_path, want.config_path);
    EXPECT_EQ(got->args, want.args);
    EXPECT_EQ(got->init, want.init);
    EXPECT_EQ(got->stop_timeout, want.stop_timeout);
    EXPECT_EQ(got->restart_policy, want.restart_policy);
    EXPECT_EQ(got->max_restart_attempts, want.max_restart_attempts);
    EXPECT_EQ(got->restart_delay, want.restart_delay);
  });
}

TEST(ContainerConfigProperty, AFieldOfTheWrongTypeIsAnError) {
  // (path into the entry, what fits there)
  enum class Kind { kText, kTexts, kBool, kUnsigned, kPolicy, kObject };
  const std::vector<std::pair<std::vector<std::string>, Kind>> fields{
      {{"name"}, Kind::kText},
      {{"command"}, Kind::kTexts},
      {{"init"}, Kind::kBool},
      {{"stop_timeout"}, Kind::kUnsigned},
      {{"restart_policy"}, Kind::kPolicy},
      {{"max_restart_attempts"}, Kind::kUnsigned},
      {{"restart_delay"}, Kind::kUnsigned}};
  const auto fits = [](const nlohmann::json& value, Kind kind) {
    const auto v = nlohmann::json::parse(value.dump());  // as read from text
    switch (kind) {
      case Kind::kText:
        return v.is_string();
      case Kind::kTexts:
        return v.is_array() && !v.empty();
      case Kind::kBool:
        return v.is_boolean();
      case Kind::kUnsigned:
        return v.is_number_unsigned();
      case Kind::kPolicy:
        return v == "no" || v == "never" || v == "always";
      case Kind::kObject:
        return v.is_object();
    }
    return false;
  };
  const std::vector<nlohmann::json> values{nullptr,
                                           true,
                                           -1,
                                           1.5,
                                           7,
                                           "text",
                                           "always",
                                           nlohmann::json::array(),
                                           nlohmann::json::object()};
  aember::test::ForAll(500, [&](Gen& g) {
    auto j = RandomContainer(g).second;
    const auto [path, kind] = g.Pick(fields);
    nlohmann::json bad;
    if (kind == Kind::kTexts && g.Chance(0.5)) {
      bad = nlohmann::json::array({"ok", 1});
    } else {
      do { bad = g.Pick(values); } while (fits(bad, kind));
    }
    j[path[0]] = bad;
    EXPECT_FALSE(Parses(j)) << path.back() << " = " << bad.dump();
  });
}

// Regression: numbers above 4294967295 were read with get<unsigned>() and
// wrapped: "max_restart_attempts": 4294967296 became 0 (= unlimited),
// "stop_timeout": 4294967306 became 10 s. Now they are refused. Found by
// these property tests (the same bug was in services.json).
TEST(ContainerConfigProperty, NumbersTooBigAreRefusedNotWrapped) {
  const std::vector<std::string> fields{
      "stop_timeout", "max_restart_attempts", "restart_delay"};
  aember::test::ForAll(200, [&](Gen& g) {
    auto j = RandomContainer(g).second;
    const auto field = g.Pick(fields);
    j[field] = g.Int<std::uint64_t>(std::uint64_t{UINT_MAX} + 1, UINT64_MAX);
    EXPECT_FALSE(Parses(j)) << field << " = " << j[field].dump();
  });
}

}  // namespace
