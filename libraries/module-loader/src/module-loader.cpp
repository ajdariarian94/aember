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

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>

#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aember::module_loader {

namespace {

// Kernel module names use '_' where file names may use '-'.
std::string Normalize(std::string_view name) {
  std::string normalized{name};
  std::ranges::replace(normalized, '-', '_');
  return normalized;
}

}  // namespace

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

  // BusyBox modprobe does not read modules.builtin, so it fails for
  // built-in modules; they need no loading.
  if (IsBuiltin(name)) {
    log_.info("Module '{}' is built into the kernel", name);
    results_.push_back({std::string{name},
                        ModuleStatus::AlreadyLoaded,
                        ModuleErrorCode::None,
                        ""});
    return {};
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

  // Attempt every module, even after a failure.
  bool all_ok = true;
  for (const auto& m : modules) {
    if (!Load(m.name).has_value()) { all_ok = false; }
  }

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

  const std::string normalized = Normalize(name);

  // Read every first token (module name) per line and compare.
  return std::ranges::any_of(
      std::ranges::istream_view<std::string>(proc_modules),
      [&](const std::string& token) { return token == normalized; });
}

// ---------------------------------------------------------------------------
// IsBuiltin
// ---------------------------------------------------------------------------

bool ModuleLoader::IsBuiltin(std::string_view name) const {
  utsname uts{};
  if (uname(&uts) != 0) { return false; }

  std::ifstream builtin{
      std::format("/lib/modules/{}/modules.builtin", uts.release)};
  if (!builtin.is_open()) { return false; }

  // Lines look like "kernel/net/llc/llc.ko".
  const std::string wanted = Normalize(name) + ".ko";
  std::string line;
  while (std::getline(builtin, line)) {
    const auto slash = line.rfind('/');
    const std::string_view file =
        slash == std::string::npos ? std::string_view{line}
                                   : std::string_view{line}.substr(slash + 1);
    if (Normalize(file) == wanted) { return true; }
  }
  return false;
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
