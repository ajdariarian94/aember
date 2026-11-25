/**
 * @file container-parser-properties.cpp
 * @author Arian Ajdari
 * @brief Property tests for containers.json (ContainersConfigParser): what
 *        is written comes back, a field of the wrong type is an error (not a
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
using aember::containerd::ContainersConfigParser;
using aember::containerd::RestartPolicy;
using aember::test::Gen;
using aember::utils::config::ConfigError;

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

std::pair<ContainerConfig, nlohmann::json> RandomContainer(Gen& g) {
  ContainerConfig c;
  c.name = "c" + AnyText(g);
  c.squashfs = "/var/lib/aember/" + AnyText(g) + ".squashfs";
  c.rootfs = "/var/lib/aember/containers/" + AnyText(g);
  c.config_path = "/var/lib/aember/" + AnyText(g) + "/lxc.container.conf";
  nlohmann::json ctr{{"squashfs", c.squashfs},
                     {"rootfs", c.rootfs},
                     {"config_path", c.config_path}};
  if (g.Chance(0.5)) {
    for (int i = g.Size(4); i > 0; --i) c.args.push_back(AnyText(g));
    ctr["args"] = c.args;
  }
  nlohmann::json j{{"name", c.name}, {"type", "container"}, {"container", ctr}};
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

bool ParseText(const std::string& text, ContainersConfigParser& parser,
               ConfigError& error) {
  const std::string path =
      std::filesystem::temp_directory_path() /
      ("aember-containers-" + std::to_string(::getpid()) + ".json");
  std::ofstream{path} << text;
  const bool ok = parser.ParseFile(path, error);
  std::filesystem::remove(path);
  return ok;
}

std::string File(const nlohmann::json& entry) {
  // Not {{"containers", {entry}}}: a one-element braced list is the element.
  return nlohmann::json{{"containers", nlohmann::json::array({entry})}}.dump();
}

TEST(ContainersConfigProperty, WhatIsWrittenComesBack) {
  aember::test::ForAll(300, [](Gen& g) {
    std::vector<ContainerConfig> want;
    auto list = nlohmann::json::array();
    for (int i = g.Size(4); i > 0; --i) {
      auto [c, j] = RandomContainer(g);
      want.push_back(c);
      list.push_back(j);
    }
    ContainersConfigParser parser;
    ConfigError error;
    ASSERT_TRUE(ParseText(
        nlohmann::json{{"containers", list}}.dump(g.Int(-1, 2)), parser, error))
        << error.message;
    ASSERT_EQ(parser.GetContainers().size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
      const auto& got = parser.GetContainers()[i];
      EXPECT_EQ(got.name, want[i].name);
      EXPECT_EQ(got.squashfs, want[i].squashfs);
      EXPECT_EQ(got.rootfs, want[i].rootfs);
      EXPECT_EQ(got.config_path, want[i].config_path);
      EXPECT_EQ(got.args, want[i].args);
      EXPECT_EQ(got.init, want[i].init);
      EXPECT_EQ(got.stop_timeout, want[i].stop_timeout);
      EXPECT_EQ(got.restart_policy, want[i].restart_policy);
      EXPECT_EQ(got.max_restart_attempts, want[i].max_restart_attempts);
      EXPECT_EQ(got.restart_delay, want[i].restart_delay);
    }
  });
}

TEST(ContainersConfigProperty, AFieldOfTheWrongTypeIsAnError) {
  // (path into the entry, what fits there)
  enum class Kind { kText, kTexts, kBool, kUnsigned, kPolicy, kObject };
  const std::vector<std::pair<std::vector<std::string>, Kind>> fields{
      {{"name"}, Kind::kText},
      {{"container"}, Kind::kObject},
      {{"container", "squashfs"}, Kind::kText},
      {{"container", "rootfs"}, Kind::kText},
      {{"container", "config_path"}, Kind::kText},
      {{"container", "args"}, Kind::kTexts},
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
        return v.is_array();
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
    (path.size() == 1 ? j[path[0]] : j[path[0]][path[1]]) = bad;
    ContainersConfigParser parser;
    ConfigError error;
    EXPECT_FALSE(ParseText(File(j), parser, error))
        << path.back() << " = " << bad.dump();
    EXPECT_FALSE(error.message.empty());
  });
}

// Regression: numbers above 4294967295 were read with get<unsigned>() and
// wrapped: "max_restart_attempts": 4294967296 became 0 (= unlimited),
// "stop_timeout": 4294967306 became 10 s. Now they are refused. Found by
// these property tests (the same bug was in services.json).
TEST(ContainersConfigProperty, NumbersTooBigAreRefusedNotWrapped) {
  const std::vector<std::string> fields{
      "stop_timeout", "max_restart_attempts", "restart_delay"};
  aember::test::ForAll(200, [&](Gen& g) {
    auto j = RandomContainer(g).second;
    const auto field = g.Pick(fields);
    j[field] = g.Int<std::uint64_t>(std::uint64_t{UINT_MAX} + 1, UINT64_MAX);
    ContainersConfigParser parser;
    ConfigError error;
    EXPECT_FALSE(ParseText(File(j), parser, error))
        << field << " = " << j[field].dump();
  });
}

}  // namespace
