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
#include <vector>

namespace aember::utils::process {

/**
 * Parses a JSON config file containing a "processes" array and returns
 * a list of ProcessConfig instances.
 *
 * Mirrors the interface of ContainersConfigParser in utils/container.
 *
 * Expected JSON shape:
 * {
 *   "processes": [
 *     {
 *       "name":               "my-service",        // required
 *       "executable":         "/usr/bin/my-svc",   // required
 *       "description":        "...",               // optional
 *       "args":               ["--port", "8080"],  // optional
 *       "working_directory":  "/var/my-svc",       // optional
 *       "environment":        { "KEY": "VALUE" },  // optional
 *       "restart_on_failure": true,                // optional, default true
 *       "max_restarts":       0,                   // optional, default 0
 * (unlimited) "restart_delay_ms":   1000,                // optional, default
 * 1000 "stop_timeout_ms":    5000                 // optional, default 5000
 *     }
 *   ]
 * }
 */
class ProcessConfigParser : public config::IConfigFileParser {
 public:
  bool ParseFile(const std::string& path, config::ConfigError& error) override;

  const std::vector<ProcessConfig>& GetProcesses() const;

 private:
  bool Parse(const nlohmann::json& json, config::ConfigError& error);

  bool ParseProcess(const nlohmann::json& json, ProcessConfig& cfg,
                    config::ConfigError& error);

 private:
  std::vector<ProcessConfig> processes_;
};

}  // namespace aember::utils::process
