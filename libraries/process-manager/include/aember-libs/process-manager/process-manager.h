/**
 * @file process-manager.h
 * @author Arian Ajdari
 * @brief ProcessManager — spawns and tracks native (non-LXC) processes.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/process/process-config-parser.h>
#include <aember-libs/utils/process/process-config.h>
#include <aember-libs/utils/process/process-state.h>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aember::process_manager {

/**
 * ProcessManager owns the lifecycle of native processes only.
 *
 * It has no knowledge of LXC, containers, or the broader service graph.
 * Container init PIDs are tracked by ContainerManager — not here.
 *
 * Responsibilities:
 *  - fork/exec a process from a ProcessConfig
 *  - maintain pid → name and name → pid mappings for native PIDs
 *  - notify the caller when a tracked process exits (ExitCallback)
 *  - schedule restarts according to ProcessConfig supervision policy
 */
class ProcessManager {
 public:
  using ProcessConfig = aember::utils::process::ProcessConfig;
  using ProcessState = aember::utils::process::ProcessState;
  using ProcessConfigParser = aember::utils::process::ProcessConfigParser;
  using Logger = aember::utils::logging::Logger;

  /**
   * Called when a tracked native process exits.
   * move_only_function — never copied.
   */
  using ExitCallback = std::move_only_function<void(
      const std::string& /*name*/, pid_t /*pid*/, int /*exit_code*/)>;

  using RestartCallback = std::move_only_function<void(const std::string&)>;

  explicit ProcessManager(ExitCallback on_exit = nullptr);
  ~ProcessManager();

  ProcessManager(const ProcessManager&) = delete;
  ProcessManager& operator=(const ProcessManager&) = delete;

  // ---------------------------------------------------------------------------
  // Registry
  // ---------------------------------------------------------------------------

  bool AddProcess(const ProcessConfig& config);
  bool RemoveProcess(std::string_view name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::optional<pid_t> Start(std::string_view name);
  bool Stop(std::string_view name);

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  /**
   * Returns true if @p pid was tracked here (cleaned up, ExitCallback fired).
   * Returns false if unknown — caller should try ContainerManager::HandleExit.
   */
  bool HandleExit(pid_t pid, int exit_code);

  // ---------------------------------------------------------------------------
  // Restart scheduling
  // ---------------------------------------------------------------------------

  /** Called internally after HandleExit when restart policy allows it. */
  void ScheduleRestart(std::string_view name);

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool HasProcess(std::string_view name) const;
  [[nodiscard]] bool IsRunning(std::string_view name) const;
  [[nodiscard]] ProcessState GetState(std::string_view name) const;
  [[nodiscard]] std::optional<pid_t> GetPid(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> GetProcessNames() const;
  [[nodiscard]] std::vector<ProcessConfig> GetConfigs() const;

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

  void SetExitCallback(ExitCallback callback);

  void SetRestartCallback(RestartCallback callback);

  // ---------------------------------------------------------------------------
  // Config loading
  // ---------------------------------------------------------------------------

  std::vector<ProcessConfig> LoadProcesses(std::string_view source);

 private:
  pid_t SpawnProcess(const ProcessConfig& config);

  std::map<std::string, ProcessConfig> configs_;
  std::map<std::string, ProcessState> states_;
  std::map<pid_t, std::string> pid_to_name_;
  std::map<std::string, pid_t> name_to_pid_;
  std::map<std::string, unsigned int> restart_counts_;

  mutable std::mutex mutex_;

  ExitCallback on_exit_;

  RestartCallback on_restart_;

  ProcessConfigParser parser_{};

  mutable Logger log_{"process-manager"};
};

}  // namespace aember::process_manager
