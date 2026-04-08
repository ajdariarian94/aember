/**
 * @file module-loader.h
 * @author Arian Ajdari
 * @brief Library definition for ModuleLoader - loads kernel modules at boot.
 * @version 0.1
 * @date 2026-03-29
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aember::module_loader {

/**
 * @brief Result of a single module load attempt.
 */
struct ModuleResult {
  std::string name;    ///< Module name e.g. "bridge"
  bool loaded{false};  ///< true if load succeeded
  std::string error;   ///< Error message if failed
};

/**
 * @class ModuleLoader
 * @brief Loads kernel modules via modprobe during early boot, after pivot
 *        to real root. Reads module list from JSON config or accepts
 *        explicit Load() calls.
 *
 * Usage:
 * @code
 *   ModuleLoader loader;
 *   loader.LoadFromConfig("/etc/aember/modules.json");
 *   // or explicitly:
 *   loader.Load("bridge");
 *   loader.Load("veth");
 *   loader.Load("br_netfilter");
 * @endcode
 */
class ModuleLoader {
 public:
  ModuleLoader();
  ~ModuleLoader() = default;

  // Non-copyable
  ModuleLoader(const ModuleLoader&) = delete;
  ModuleLoader& operator=(const ModuleLoader&) = delete;

  /**
   * @brief Load a single kernel module by name via modprobe.
   * @param name  Module name e.g. "bridge", "veth", "br_netfilter"
   * @return true if module loaded successfully or was already loaded.
   */
  bool Load(const std::string& name);

  /**
   * @brief Load all modules listed in a JSON config file.
   * @param config_path  Path to JSON file containing module list.
   * @return true if all modules loaded successfully.
   */
  bool LoadFromConfig(const std::string& config_path);

  /**
   * @brief Load all modules from a parsed JSON object.
   * @param config  JSON object with "modules" array of strings.
   * @return true if all modules loaded successfully.
   */
  bool LoadFromJson(const nlohmann::json& config);

  /**
   * @brief Check if a module is currently loaded.
   * @param name  Module name.
   * @return true if module appears in /proc/modules.
   */
  bool IsLoaded(const std::string& name) const;

  /**
   * @brief Returns results of all load attempts since construction.
   */
  const std::vector<ModuleResult>& GetResults() const { return results_; }

 private:
  /**
   * @brief Runs modprobe for the given module name.
   * @return Exit code, 0 on success.
   */
  int RunModprobe(const std::string& name);

  std::vector<ModuleResult> results_;  ///< Results of all load attempts
  aember::utils::Logger log_{"module-loader"};
};

}  // namespace aember::module_loader
