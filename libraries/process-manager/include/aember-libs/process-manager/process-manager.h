/**
 * @file process-manager.h
 * @author Arian Ajdari
 * @brief ProcessManager — spawns and tracks native (non-LXC) processes.
 * @version 0.2
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
 *  - maintain the pid → name and name → pid mappings for native PIDs
 *  - notify the caller when a tracked process exits (ExitCallback)
 *  - schedule restarts according to ProcessConfig supervision policy
 *
 * ProcessManager does NOT:
 *  - know about service dependencies
 *  - touch anything LXC-related
 *  - own ContainerManager or ServiceManager concerns
 */
class ProcessManager {
 public:
  using ProcessConfig = aember::utils::process::ProcessConfig;
  using ProcessState = aember::utils::process::ProcessState;
  using ProcessConfigParser = aember::utils::process::ProcessConfigParser;
  using Logger = aember::utils::logging::Logger;

  /**
   * Called when a tracked native process exits.
   *
   * @param name      Name that owned the process.
   * @param pid       PID that exited.
   * @param exit_code Waitpid exit status (use WEXITSTATUS / WIFSIGNALED).
   */
  using ExitCallback = std::function<void(const std::string& /*name*/,
                                          pid_t /*pid*/, int /*exit_code*/)>;

  explicit ProcessManager(ExitCallback on_exit = nullptr);
  ~ProcessManager();

  ProcessManager(const ProcessManager&) = delete;
  ProcessManager& operator=(const ProcessManager&) = delete;

  // ---------------------------------------------------------------------------
  // Registry
  // ---------------------------------------------------------------------------

  /**
   * Register a process config without starting it.
   * Returns false if a config with this name already exists.
   */
  bool AddProcess(const ProcessConfig& config);

  /** Deregister a process. Stops it first if running. */
  bool RemoveProcess(const std::string& name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  /**
   * Spawn the native process registered under @p name.
   *
   * @return The PID of the spawned process, or std::nullopt on failure.
   *
   * The PID is immediately tracked; HandleExit cleans it up on exit.
   */
  [[nodiscard]] std::optional<pid_t> Start(const std::string& name);

  /**
   * Send SIGTERM to the process registered under @p name, followed by
   * SIGKILL after ProcessConfig::stop_timeout if it has not yet exited.
   *
   * Returns false if no native process is running under @p name.
   */
  bool Stop(const std::string& name);

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  /**
   * Called by ServiceManager when ChildSupervisor delivers an exit event.
   *
   * Returns true if @p pid was a native process tracked here (entry is
   * cleaned up and ExitCallback fired). Returns false if the PID is
   * unknown — the caller should then try ContainerManager::HandleExit.
   */
  bool HandleExit(pid_t pid, int exit_code);

  // ---------------------------------------------------------------------------
  // Restart scheduling
  // ---------------------------------------------------------------------------

  /**
   * Schedule a restart of @p name according to its ProcessConfig restart
   * policy (restart_on_failure, max_restarts, restart_delay).
   *
   * Called internally after HandleExit when the policy allows a restart.
   * A second call while a restart is already pending is a no-op.
   */
  void ScheduleRestart(const std::string& name);

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool HasProcess(const std::string& name) const;
  [[nodiscard]] bool IsRunning(const std::string& name) const;
  [[nodiscard]] ProcessState GetState(const std::string& name) const;
  [[nodiscard]] std::optional<pid_t> GetPid(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> GetProcessNames() const;
  [[nodiscard]] std::vector<ProcessConfig> GetConfigs() const;

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

  void SetExitCallback(ExitCallback callback);

  // ---------------------------------------------------------------------------
  // Config loading
  // ---------------------------------------------------------------------------

  /** Parse process configs from a named config source (file, dir, …). */
  std::vector<ProcessConfig> LoadProcesses(const std::string& source);

 private:
  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  pid_t SpawnProcess(const ProcessConfig& config);

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  std::map<std::string, ProcessConfig> configs_;        ///< registered configs
  std::map<std::string, ProcessState> states_;          ///< per-process state
  std::map<pid_t, std::string> pid_to_name_;            ///< native PIDs only
  std::map<std::string, pid_t> name_to_pid_;            ///< reverse index
  std::map<std::string, unsigned int> restart_counts_;  ///< attempts so far

  mutable std::mutex mutex_;

  ExitCallback on_exit_;

  ProcessConfigParser parser_{};

  mutable Logger log_{"process-manager"};
};

}  // namespace aember::process_manager
