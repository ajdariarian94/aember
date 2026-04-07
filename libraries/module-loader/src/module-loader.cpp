/**
 * @file module-loader.cpp
 * @author Arian Ajdari
 * @brief Library implementation for ModuleLoader
 * @version 0.1
 * @date 2026-03-29
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/module-loader/module-loader.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace aember::module_loader {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ModuleLoader::ModuleLoader() {
  log_.info("ModuleLoader initialized");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ModuleLoader::Load(const std::string& name) {
  if (name.empty()) {
    log_.error("Cannot load module with empty name");
    return false;
  }

  // Skip if already loaded
  if (IsLoaded(name)) {
    log_.debug("Module '{}' already loaded, skipping", name);
    results_.push_back({name, true, "already loaded"});
    return true;
  }

  log_.info("Loading module '{}'", name);

  int ret = RunModprobe(name);

  ModuleResult result;
  result.name = name;
  result.loaded = (ret == 0);

  if (ret == 0) {
    log_.info("Module '{}' loaded successfully", name);
  } else {
    result.error = "modprobe exited with code " + std::to_string(ret);
    log_.error("Failed to load module '{}': {}", name, result.error);
  }

  results_.push_back(result);
  return result.loaded;
}

bool ModuleLoader::LoadFromConfig(const std::string& config_path) {
  log_.info("Loading modules from config '{}'", config_path);

  std::ifstream f(config_path);
  if (!f.is_open()) {
    log_.error("Cannot open module config '{}'", config_path);
    return false;
  }

  nlohmann::json config;
  try {
    f >> config;
  } catch (const std::exception& e) {
    log_.error("Failed to parse module config '{}': {}", config_path, e.what());
    return false;
  }

  return LoadFromJson(config);
}

bool ModuleLoader::LoadFromJson(const nlohmann::json& config) {
  if (!config.contains("modules") || !config["modules"].is_array()) {
    log_.error("Module config missing 'modules' array");
    return false;
  }

  bool all_ok = true;
  for (const auto& entry : config["modules"]) {
    std::string name = entry.get<std::string>();
    if (!Load(name)) { all_ok = false; }
  }

  log_.info("Module loading complete ({} module(s), {} failed)",
            results_.size(),
            std::count_if(results_.begin(),
                          results_.end(),
                          [](const ModuleResult& r) { return !r.loaded; }));

  return all_ok;
}

bool ModuleLoader::IsLoaded(const std::string& name) const {
  std::ifstream proc_modules("/proc/modules");
  if (!proc_modules.is_open()) { return false; }

  // Module names use underscores, but modprobe accepts dashes too
  std::string normalized = name;
  std::replace(normalized.begin(), normalized.end(), '-', '_');

  std::string line;
  while (std::getline(proc_modules, line)) {
    // Format: module_name size refcount deps state address
    std::istringstream ss(line);
    std::string mod_name;
    ss >> mod_name;

    if (mod_name == normalized) { return true; }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

int ModuleLoader::RunModprobe(const std::string& name) {
  pid_t pid = fork();

  if (pid < 0) {
    log_.error("fork() failed for modprobe: {}", strerror(errno));
    return -1;
  }

  if (pid == 0) {
    // Child — redirect stdout/stderr to /dev/null
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("modprobe", "modprobe", name.c_str(), nullptr);
    _exit(127);  // exec failed
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace aember::module_loader
