/**
 * @file module-loader.cpp
 * @author Arian Ajdari
 * @brief Library implementation for ModuleLoader
 * @version 0.1
 * @date 2026-03-29
 */

#include <aember-libs/module-loader/module-loader.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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

    results_.push_back({name,
                        ModuleStatus::Failed,
                        ModuleErrorCode::InvalidInput,
                        "empty module name"});

    return false;
  }

  // Already loaded
  if (IsLoaded(name)) {
    log_.debug("Module '{}' already loaded, skipping", name);

    results_.push_back(
        {name, ModuleStatus::AlreadyLoaded, ModuleErrorCode::None, ""});

    return true;
  }

  log_.info("Loading module '{}'", name);

  int ret = RunModprobe(name);

  ModuleResult result;
  result.name = name;

  if (ret == 0) {
    result.status = ModuleStatus::Loaded;
    result.code = ModuleErrorCode::None;

    log_.info("Module '{}' loaded successfully", name);
  } else {
    result.status = ModuleStatus::Failed;
    result.code = ModuleErrorCode::ModprobeFailed;
    result.error = "modprobe exited with code " + std::to_string(ret);

    log_.error("Failed to load module '{}': {}", name, result.error);
  }

  results_.push_back(result);
  return result.status != ModuleStatus::Failed;
}

// ---------------------------------------------------------------------------
// Bulk load
// ---------------------------------------------------------------------------

bool ModuleLoader::Load(const std::vector<ModuleConfig>& modules) {
  if (modules.empty()) {
    log_.warn("No modules to load");
    return true;
  }

  bool all_ok = true;

  for (const auto& m : modules) {
    if (!Load(m.name)) { all_ok = false; }
  }

  const auto failed = std::count_if(
      results_.begin(), results_.end(), [](const ModuleResult& r) {
        return r.status == ModuleStatus::Failed;
      });

  log_.info(
      "Module loading complete ({} total, {} failed)", results_.size(), failed);

  return all_ok;
}

// ---------------------------------------------------------------------------
// Parser bridge (same pattern as ContainerManager)
// ---------------------------------------------------------------------------

std::vector<ModuleLoader::ModuleConfig> ModuleLoader::LoadModules(
    const std::string& path) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(path, error)) {
    log_.error("Failed to parse module config '{}': {}", path, error.message);
    return {};
  }

  auto modules = parser_.GetModules();

  if (modules.empty()) { log_.warn("No modules found in '{}'", path); }

  return modules;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ModuleLoader::IsLoaded(const std::string& name) const {
  std::ifstream proc_modules("/proc/modules");
  if (!proc_modules.is_open()) { return false; }

  // Normalize: '-' → '_'
  std::string normalized = name;
  std::replace(normalized.begin(), normalized.end(), '-', '_');

  std::string line;
  while (std::getline(proc_modules, line)) {
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
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("modprobe", "modprobe", name.c_str(), nullptr);
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    log_.error("waitpid() failed: {}", strerror(errno));
    return -1;
  }

  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace aember::module_loader
