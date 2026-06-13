/**
 * @file process-state.h
 * @author Arian Ajdari
 * @brief ProcessState — lifecycle states for a native process.
 *        Moved and renamed from utils/service/service-state.h.
 * @version 0.1
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <string_view>

namespace aember::utils::process {

/**
 * Lifecycle states tracked by ProcessManager for each native process.
 *
 * Valid transitions:
 *
 *   Stopped ──► Starting ──► Running ──► Stopping ──► Stopped
 *                                  └──► Failed
 *                                  └──► Restarting ──► Starting
 */
enum class ProcessState {
  Stopped,     ///< Not running; either never started or cleanly stopped.
  Starting,    ///< Spawn requested; waiting for the process to be ready.
  Running,     ///< Process is alive and considered healthy.
  Stopping,    ///< SIGTERM sent; waiting for the process to exit.
  Restarting,  ///< Exited unexpectedly; restart timer is running.
  Failed,      ///< Exited and max_restarts exhausted (or restart disabled).
};

/// Human-readable name for logging and diagnostics.
constexpr std::string_view ToString(ProcessState s) noexcept {
  switch (s) {
    case ProcessState::Stopped:
      return "stopped";
    case ProcessState::Starting:
      return "starting";
    case ProcessState::Running:
      return "running";
    case ProcessState::Stopping:
      return "stopping";
    case ProcessState::Restarting:
      return "restarting";
    case ProcessState::Failed:
      return "failed";
  }
  return "unknown";
}

}  // namespace aember::utils::process
