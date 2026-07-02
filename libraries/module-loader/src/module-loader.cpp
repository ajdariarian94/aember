/**
 * @file module-loader.cpp
 * @author Arian Ajdari
 * @brief ModuleLoader implementation.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/module-loader/module-loader.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>

namespace aember::module_loader {

// ---------------------------------------------------------------------------
// Ctor
// ---------------------------------------------------------------------------

ModuleLoader::ModuleLoader() {
  log_.info("ModuleLoader initialized");
}

// ---------------------------------------------------------------------------
// Load — single module
// ---------------------------------------------------------------------------

ModuleLoader::Result<> ModuleLoader::Load(std::string_view name) {
  if (name.empty()) {
    const Error err{"empty module name", ModuleErrorCode::InvalidInput};
    results_.push_back(
        {std::string{name}, ModuleStatus::Failed, err.code, err.message});
    return std::unexpected(err);
  }

  if (IsLoaded(name)) {
    log_.debug("Module '{}' already loaded, skipping", name);
    results_.push_back({std::string{name},
                        ModuleStatus::AlreadyLoaded,
                        ModuleErrorCode::None,
                        ""});
    return {};
  }

  log_.info("Loading module '{}'", name);

  const int ret = RunModprobe(name);

  if (ret == 0) {
    results_.push_back(
        {std::string{name}, ModuleStatus::Loaded, ModuleErrorCode::None, ""});
    log_.info("Module '{}' loaded successfully", name);
    return {};
  }

  const Error err{std::format("modprobe exited with code {}", ret),
                  ModuleErrorCode::ModprobeFailed};
  results_.push_back(
      {std::string{name}, ModuleStatus::Failed, err.code, err.message});
  log_.error("Failed to load module '{}': {}", name, err.message);
  return std::unexpected(err);
}

// ---------------------------------------------------------------------------
// Load — bulk
// ---------------------------------------------------------------------------

bool ModuleLoader::Load(const std::vector<ModuleConfig>& modules) {
  if (modules.empty()) {
    log_.warn("No modules to load");
    return true;
  }

  const bool all_ok = std::ranges::all_of(
      modules, [this](const auto& m) { return Load(m.name).has_value(); });

  const auto failed = std::ranges::count_if(
      results_, [](const auto& r) { return r.status == ModuleStatus::Failed; });

  log_.info(
      "Module loading complete ({} total, {} failed)", results_.size(), failed);

  return all_ok;
}

// ---------------------------------------------------------------------------
// LoadModules
// ---------------------------------------------------------------------------

std::vector<ModuleLoader::ModuleConfig> ModuleLoader::LoadModules(
    std::string_view path) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(std::string{path}, error)) {
    log_.error("Failed to parse module config '{}': {}", path, error.message);
    return {};
  }

  auto modules = parser_.GetModules();

  if (modules.empty()) { log_.warn("No modules found in '{}'", path); }

  return modules;
}

// ---------------------------------------------------------------------------
// IsLoaded
// ---------------------------------------------------------------------------

bool ModuleLoader::IsLoaded(std::string_view name) const {
  std::ifstream proc_modules{"/proc/modules"};
  if (!proc_modules.is_open()) { return false; }

  // Normalise: kernel replaces '-' with '_' in module names.
  std::string normalized{name};
  std::ranges::replace(normalized, '-', '_');

  // Read every first token (module name) per line and compare.
  return std::ranges::any_of(
      std::ranges::istream_view<std::string>(proc_modules),
      [&](const std::string& token) { return token == normalized; });
}

// ---------------------------------------------------------------------------
// RunModprobe
// ---------------------------------------------------------------------------

int ModuleLoader::RunModprobe(std::string_view name) {
  const pid_t pid = fork();

  if (pid < 0) {
    log_.error("fork() failed: {}", strerror(errno));
    return -1;
  }

  if (pid == 0) {
    // Child: silence stdout/stderr then exec modprobe.
    if (const int devnull = open("/dev/null", O_WRONLY); devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    // name is valid for the lifetime of this call — safe to c_str().
    execlp("modprobe", "modprobe", std::string{name}.c_str(), nullptr);
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
