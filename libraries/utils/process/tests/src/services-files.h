/**
 * @file services-files.h
 * @brief Tests write services the way a device has them: services.json
 *        (names, dependencies) and one folder per service with its
 *        config.json. They describe a service as one entry with everything
 *        ({"processes": [{"name", "executable", "dependencies", ...}]}),
 *        which this splits up.
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/process/process-config-parser.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace services_test {

namespace fs = std::filesystem;

/// Parse @p text (one-entry-per-service form, or anything else as it is)
/// as services.json with its folders under @p dir. Success means: the file
/// parsed and no service was skipped (a skipped one's reason is the error).
inline bool Parse(const fs::path& dir, const std::string& text,
                  aember::utils::process::ProcessConfigParser& parser,
                  aember::utils::config::ConfigError& error) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "services");
  std::string file = text;
  const auto json = nlohmann::json::parse(text, nullptr, false);
  if (json.is_object() && json.contains("processes") &&
      json["processes"].is_array()) {
    auto services = nlohmann::json::array();
    for (const auto& entry : json["processes"]) {
      if (!entry.is_object() || !entry.contains("name") ||
          !entry["name"].is_string()) {
        services.push_back(entry);  // the parser refuses it
        continue;
      }
      const auto name = entry["name"].get<std::string>();
      nlohmann::json item{{"name", name}};
      auto config = entry;
      if (config.contains("dependencies")) {
        item["dependencies"] = config["dependencies"];
        config.erase("dependencies");
      }
      services.push_back(item);
      const auto folder = dir / "services" / name;
      if (name.empty() || name.find("..") != std::string::npos) continue;
      fs::create_directories(folder, ec);
      if (!ec) std::ofstream{folder / "config.json"} << config.dump();
    }
    file = nlohmann::json{{"services", services}}.dump();
  }
  std::ofstream{dir / "services.json"} << file;
  parser.SetServicesDir((dir / "services").string());
  error = {};
  const bool ok = parser.ParseFile((dir / "services.json").string(), error);
  if (ok && !parser.GetSkipped().empty()) {
    error = aember::utils::config::ConfigError(parser.GetSkipped()[0].second);
    return false;
  }
  return ok;
}

}  // namespace services_test
