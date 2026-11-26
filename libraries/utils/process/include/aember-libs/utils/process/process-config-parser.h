/**
 * @file process-config-parser.h
 * @author Arian Ajdari
 * @brief ProcessConfigParser — parses process config files into ProcessConfig
 *        instances. Adapted from utils/service/service-parser.h.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/config/iconfig-file-parser.h>
#include <aember-libs/utils/process/process-config.h>

#include <string>
#include <utility>
#include <vector>

namespace aember::utils::process {

/// Where each service's own folder is: <dir>/<name>/config.json.
inline constexpr const char* kServicesDir = "/usr/lib/aember/services";

/**
 * services.json (a board's: which services start and what each one needs
 * first) and each service's config.json (its own folder, installed with its
 * program):
 *
 *   /etc/aember/services.json
 *   {"services": [{"name": "aember-netd"},
 *                 {"name": "aember-containerd",
 *                  "dependencies": ["aember-netd"]}]}
 *
 *   /usr/lib/aember/services/aember-netd/config.json
 *   {"name": "aember-netd", "executable": "/usr/bin/aember-netd",   // required
 *    "description": "...", "args": [...], "working_directory": "...",
 *    "environment": {"K": "V"}, "restart_on_failure": true,
 *    "max_restarts": 0, "restart_delay_ms": 1000, "stop_timeout_ms": 5000,
 *    "ipc": false, "ready_timeout_ms": 30000}
 *
 * A malformed services.json is an error (nothing starts). A service whose
 * config.json is missing or invalid is skipped (GetSkipped(): name, why);
 * the others are unaffected, and whatever depends on it doesn't start.
 */
class ProcessConfigParser : public config::IConfigFileParser {
 public:
  bool ParseFile(const std::string& path, config::ConfigError& error) override;

  const std::vector<ProcessConfig>& GetProcesses() const;
  /// Services listed but not loaded: (name, why).
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>&
  GetSkipped() const {
    return skipped_;
  }
  /// Where the services' folders are (kServicesDir on a device).
  void SetServicesDir(std::string dir) { services_dir_ = std::move(dir); }

  /// One service's config.json (its settings; "dependencies" belong in
  /// services.json and are refused here).
  static bool ParseServiceConfig(const nlohmann::json& json, ProcessConfig& cfg,
                                 config::ConfigError& error);

 private:
  bool Parse(const nlohmann::json& json, config::ConfigError& error);

  static bool ParseProcess(const nlohmann::json& json, ProcessConfig& cfg,
                           config::ConfigError& error);

 private:
  std::vector<ProcessConfig> processes_;
  std::vector<std::pair<std::string, std::string>> skipped_;
  std::string services_dir_{kServicesDir};
};

}  // namespace aember::utils::process
