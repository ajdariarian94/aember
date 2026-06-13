/**
 * @file process-config.h
 * @author Arian Ajdari
 * @brief ProcessConfig — configuration for a native (non-LXC) process.
 *        Moved and renamed from utils/service/service-config.h.
 * @version 0.1
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace aember::utils::process {

/**
 * Describes everything ProcessManager needs to spawn and supervise a
 * native process. No LXC or container fields belong here.
 */
struct ProcessConfig {
  // ---------------------------------------------------------------------------
  // Identity
  // ---------------------------------------------------------------------------

  /// Unique name used as the process's handle throughout the system.
  std::string name;

  /// Description shown in logs and diagnostics.
  std::string description;

  // ---------------------------------------------------------------------------
  // Executable
  // ---------------------------------------------------------------------------

  /// Absolute path to the executable.
  std::string executable;

  /// Argument list passed to execv (argv[0] is set automatically to
  /// executable).
  std::vector<std::string> args;

  /// Working directory for the spawned process. Empty = inherit from parent.
  std::string working_directory;

  /// Additional environment variables merged with the parent environment at
  /// spawn time via setenv(). Key = variable name, value = variable value.
  std::map<std::string, std::string> environment;

  // ---------------------------------------------------------------------------
  // Supervision
  // ---------------------------------------------------------------------------

  /// Whether ProcessManager should restart this process on unexpected exit.
  bool restart_on_failure{true};

  /// Services that must be running before this process is started.
  std::vector<std::string> dependencies;

  /// Maximum number of restart attempts before giving up (0 = unlimited).
  unsigned int max_restarts{0};

  /// Delay between a process exit and the next restart attempt.
  std::chrono::milliseconds restart_delay{std::chrono::seconds(1)};

  // ---------------------------------------------------------------------------
  // Shutdown
  // ---------------------------------------------------------------------------

  /// Grace period after SIGTERM before SIGKILL is sent.
  std::chrono::milliseconds stop_timeout{std::chrono::seconds(5)};

  ProcessConfig() = default;
  ProcessConfig(const std::string& name, const std::string& executable);
};

}  // namespace aember::utils::process
