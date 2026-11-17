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
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
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

ModuleLoader::ModuleLoader(std::filesystem::path modules_dir)
    : modules_dir_(std::move(modules_dir)) {
  if (modules_dir_.empty()) {
    utsname uts{};
    if (uname(&uts) == 0) {
      modules_dir_ = std::filesystem::path{"/lib/modules"} / uts.release;
    }
  }
  log_.info("ModuleLoader initialized ({})", modules_dir_.string());
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

  // Built-in modules need no loading.
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

  const auto order = LoadOrder(name);
  if (order.empty()) {
    const Error err{std::format("not in {}/modules.dep", modules_dir_.string()),
                    ModuleErrorCode::NotFound};
    results_.push_back(
        {std::string{name}, ModuleStatus::Failed, err.code, err.message});
    log_.error("Failed to load module '{}': {}", name, err.message);
    return std::unexpected(err);
  }

  // Dependencies first, then the module itself (last in the order).
  for (const auto& rel : order) {
    const int rc = LoadFile(modules_dir_ / rel);
    if (rc == 0 || rc == -EEXIST) { continue; }
    const Error err{std::format("finit_module {}: {}", rel, strerror(-rc)),
                    ModuleErrorCode::LoadFailed};
    results_.push_back(
        {std::string{name}, ModuleStatus::Failed, err.code, err.message});
    log_.error("Failed to load module '{}': {}", name, err.message);
    return std::unexpected(err);
  }

  results_.push_back(
      {std::string{name}, ModuleStatus::Loaded, ModuleErrorCode::None, ""});
  log_.info("Module '{}' loaded successfully", name);
  return {};
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

  // Compare the first token (module name) of each line only; the other
  // columns ("Live", sizes, addresses) must not match.
  std::string line;
  while (std::getline(proc_modules, line)) {
    if (line.substr(0, line.find(' ')) == normalized) { return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------
// IsBuiltin
// ---------------------------------------------------------------------------

bool ModuleLoader::IsBuiltin(std::string_view name) const {
  std::ifstream builtin{modules_dir_ / "modules.builtin"};
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
// modules.dep, finit_module (what modprobe did)
// ---------------------------------------------------------------------------

namespace {

/// "kernel/drivers/gpu/drm/virtio/virtio-gpu.ko" -> "virtio_gpu"
std::string ModuleNameOf(std::string_view rel) {
  const auto slash = rel.rfind('/');
  auto file = slash == std::string_view::npos ? rel : rel.substr(slash + 1);
  file = file.substr(0, file.find(".ko"));
  return Normalize(file);
}

}  // namespace

const std::unordered_map<std::string, ModuleLoader::DepEntry>&
ModuleLoader::DepMap() {
  if (dep_map_) { return *dep_map_; }
  dep_map_.emplace();

  std::ifstream dep{modules_dir_ / "modules.dep"};
  if (!dep.is_open()) {
    log_.warn("No {}/modules.dep", modules_dir_.string());
    return *dep_map_;
  }

  // "kernel/a/b.ko: kernel/c/d.ko kernel/e/f.ko"
  std::string line;
  while (std::getline(dep, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) { continue; }
    DepEntry entry{line.substr(0, colon), {}};
    std::istringstream rest{line.substr(colon + 1)};
    for (std::string d; rest >> d;) { entry.deps.push_back(std::move(d)); }
    (*dep_map_)[ModuleNameOf(entry.path)] = std::move(entry);
  }
  return *dep_map_;
}

std::vector<std::string> ModuleLoader::LoadOrder(std::string_view name) {
  const auto& map = DepMap();
  const auto it = map.find(Normalize(name));
  if (it == map.end()) { return {}; }

  // modules.dep lists the full transitive closure of each module, deepest
  // dependencies last; load each dependency's own dependencies before it.
  std::vector<std::string> order;
  std::vector<std::string> visiting;  // guards against a cyclic modules.dep
  const auto visit = [&](auto&& self, const DepEntry& entry) -> void {
    if (std::ranges::find(order, entry.path) != order.end()) { return; }
    if (std::ranges::find(visiting, entry.path) != visiting.end()) { return; }
    visiting.push_back(entry.path);
    for (const auto& d : entry.deps) {
      if (const auto dep = map.find(ModuleNameOf(d)); dep != map.end()) {
        self(self, dep->second);
      }
    }
    if (std::ranges::find(order, entry.path) == order.end()) {
      order.push_back(entry.path);
    }
  };
  visit(visit, it->second);
  return order;
}

int ModuleLoader::LoadFile(const std::filesystem::path& ko) {
  const int fd = ::open(ko.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { return -errno; }
  const long rc = ::syscall(SYS_finit_module, fd, "", 0);
  const int err = rc == 0 ? 0 : -errno;
  ::close(fd);
  return err;
}

}  // namespace aember::module_loader
