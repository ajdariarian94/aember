/**
 * @file process-config-properties.cpp
 * @author Arian Ajdari
 * @brief Property tests for services.json (ProcessConfigParser): what is
 *        written comes back, a field of the wrong type is an error (never
 *        a crash or a silent default), numbers out of range are refused.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/process/process-config-parser.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "services-files.h"
#include <aember-test/property.h>

namespace {

using aember::test::Gen;
using aember::utils::config::ConfigError;
using aember::utils::process::ProcessConfig;
using aember::utils::process::ProcessConfigParser;

/// Text JSON can carry: quotes, backslashes, control characters, UTF-8.
std::string AnyText(Gen& g) {
  static const std::vector<std::string> pieces{
      "a", "b", "1", "-", "/", "\"", "\\", "\n", "\t", "\x01", " ", "\xc3\xa9"};
  std::string s;
  for (int i = g.Size(10); i > 0; --i) s += g.Pick(pieces);
  return s;
}

unsigned int AnyUnsigned(Gen& g) {
  return g.Chance(0.2) ? g.Pick(std::vector<unsigned int>{0, 1, UINT_MAX})
                       : g.Int<unsigned int>(0, 100000);
}

/// A random process and its services.json entry; optional fields are left
/// out sometimes (the parser's defaults then apply).
std::pair<ProcessConfig, nlohmann::json> RandomProcess(Gen& g) {
  ProcessConfig c;
  nlohmann::json j;
  // A name that is also a folder name (each service has its own folder).
  c.name = "s" + g.Text("abcdefghijklmnopqrstuvwxyz0123456789-_", 0, 12);
  c.executable = "/usr/bin/" + AnyText(g);
  j["name"] = c.name;
  j["executable"] = c.executable;
  if (g.Chance(0.5)) j["description"] = c.description = AnyText(g);
  if (g.Chance(0.5)) {
    for (int i = g.Size(4); i > 0; --i) c.args.push_back(AnyText(g));
    j["args"] = c.args;
  }
  if (g.Chance(0.5)) j["working_directory"] = c.working_directory = AnyText(g);
  if (g.Chance(0.5)) {
    for (int i = g.Size(3); i > 0; --i) c.environment[AnyText(g)] = AnyText(g);
    j["environment"] = c.environment;
  }
  if (g.Chance(0.5)) {
    for (int i = g.Size(3); i > 0; --i) c.dependencies.push_back(AnyText(g));
    j["dependencies"] = c.dependencies;
  }
  if (g.Chance(0.5))
    j["restart_on_failure"] = c.restart_on_failure = g.Chance(0.5);
  if (g.Chance(0.5)) j["max_restarts"] = c.max_restarts = AnyUnsigned(g);
  if (g.Chance(0.5)) {
    const auto ms = AnyUnsigned(g);
    j["restart_delay_ms"] = ms;
    c.restart_delay = std::chrono::milliseconds{ms};
  }
  if (g.Chance(0.5)) {
    const auto ms = AnyUnsigned(g);
    j["stop_timeout_ms"] = ms;
    c.stop_timeout = std::chrono::milliseconds{ms};
  }
  if (g.Chance(0.5)) j["ipc"] = c.ipc = g.Chance(0.5);
  if (g.Chance(0.5)) {
    const auto ms = AnyUnsigned(g);
    j["ready_timeout_ms"] = ms;
    c.ready_timeout = std::chrono::milliseconds{ms};
  }
  return {c, j};
}

/// Parse @p text as services.json with one folder per service.
bool ParseText(const std::string& text, ProcessConfigParser& parser,
               ConfigError& error) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("aember-services-" + std::to_string(::getpid()));
  const bool ok = services_test::Parse(dir, text, parser, error);
  std::filesystem::remove_all(dir);
  return ok;
}

void ExpectSame(const ProcessConfig& got, const ProcessConfig& want) {
  EXPECT_EQ(got.name, want.name);
  EXPECT_EQ(got.executable, want.executable);
  EXPECT_EQ(got.description, want.description);
  EXPECT_EQ(got.args, want.args);
  EXPECT_EQ(got.working_directory, want.working_directory);
  EXPECT_EQ(got.environment, want.environment);
  EXPECT_EQ(got.dependencies, want.dependencies);
  EXPECT_EQ(got.restart_on_failure, want.restart_on_failure);
  EXPECT_EQ(got.max_restarts, want.max_restarts);
  EXPECT_EQ(got.restart_delay, want.restart_delay);
  EXPECT_EQ(got.stop_timeout, want.stop_timeout);
  EXPECT_EQ(got.ipc, want.ipc);
  EXPECT_EQ(got.ready_timeout, want.ready_timeout);
}

TEST(ProcessConfigProperty, WhatIsWrittenComesBack) {
  aember::test::ForAll(300, [](Gen& g) {
    std::vector<ProcessConfig> want;
    nlohmann::json list = nlohmann::json::array();
    for (int i = g.Size(4); i > 0; --i) {
      auto [c, j] = RandomProcess(g);
      c.name += "-" + std::to_string(i);  // one folder each
      j["name"] = c.name;
      want.push_back(c);
      list.push_back(j);
    }
    ProcessConfigParser parser;
    ConfigError error{""};
    ASSERT_TRUE(ParseText(
        nlohmann::json{{"processes", list}}.dump(g.Int(-1, 2)), parser, error))
        << error.message;
    ASSERT_EQ(parser.GetProcesses().size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
      ExpectSame(parser.GetProcesses()[i], want[i]);
    }
  });
}

/// One entry in a services.json file.
std::string File(const nlohmann::json& entry) {
  // Not {{"processes", {entry}}}: a one-element braced list is the element.
  return nlohmann::json{{"processes", nlohmann::json::array({entry})}}.dump();
}

TEST(ProcessConfigProperty, AFieldOfTheWrongTypeIsAnError) {
  enum class Kind { kText, kTexts, kTextMap, kBool, kUnsigned };
  const std::vector<std::pair<std::string, Kind>> fields{
      {"name", Kind::kText},
      {"executable", Kind::kText},
      {"description", Kind::kText},
      {"working_directory", Kind::kText},
      {"args", Kind::kTexts},
      {"dependencies", Kind::kTexts},
      {"environment", Kind::kTextMap},
      {"restart_on_failure", Kind::kBool},
      {"ipc", Kind::kBool},
      {"max_restarts", Kind::kUnsigned},
      {"restart_delay_ms", Kind::kUnsigned},
      {"stop_timeout_ms", Kind::kUnsigned},
      {"ready_timeout_ms", Kind::kUnsigned}};
  // Judged on the value as the parser reads it back from text: 7 written
  // from an int is unsigned once parsed.
  const auto fits = [](const nlohmann::json& value, Kind kind) {
    const auto v = nlohmann::json::parse(value.dump());
    switch (kind) {
      case Kind::kText:
        return v.is_string();
      case Kind::kTexts:
        return v.is_array();  // elements: below
      case Kind::kTextMap:
        return v.is_object();
      case Kind::kBool:
        return v.is_boolean();
      case Kind::kUnsigned:
        return v.is_number_unsigned();
    }
    return false;
  };
  const std::vector<nlohmann::json> values{nullptr,
                                           true,
                                           -1,
                                           1.5,
                                           7,
                                           "text",
                                           nlohmann::json::array(),
                                           nlohmann::json::object()};
  aember::test::ForAll(500, [&](Gen& g) {
    auto j = RandomProcess(g).second;
    const auto [field, kind] = g.Pick(fields);
    nlohmann::json bad;
    if (kind == Kind::kTexts && g.Chance(0.5)) {
      bad = nlohmann::json::array({"ok", 1});  // an element of another type
    } else if (kind == Kind::kTextMap && g.Chance(0.5)) {
      bad = nlohmann::json::object({{"K", 1}});
    } else {
      do { bad = g.Pick(values); } while (fits(bad, kind));
    }
    j[field] = bad;
    ProcessConfigParser parser;
    ConfigError error{""};
    EXPECT_FALSE(ParseText(File(j), parser, error))
        << field << " = " << bad.dump();
    EXPECT_FALSE(error.message.empty());
  });
}

// Regression: numbers above 4294967295 were read with get<unsigned int>()
// and wrapped: "max_restarts": 4294967296 became 0, i.e. unlimited, and
// "restart_delay_ms": 4294967297 became 1 ms. Now they are refused. Found by
// these property tests.
TEST(ProcessConfigProperty, NumbersTooBigAreRefusedNotWrapped) {
  const std::vector<std::string> fields{"max_restarts",
                                        "restart_delay_ms",
                                        "stop_timeout_ms",
                                        "ready_timeout_ms"};
  aember::test::ForAll(200, [&](Gen& g) {
    auto j = RandomProcess(g).second;
    const auto field = g.Pick(fields);
    j[field] = g.Int<std::uint64_t>(std::uint64_t{UINT_MAX} + 1, UINT64_MAX);
    ProcessConfigParser parser;
    ConfigError error{""};
    EXPECT_FALSE(ParseText(File(j), parser, error))
        << field << " = " << j[field].dump();
  });
}

}  // namespace
