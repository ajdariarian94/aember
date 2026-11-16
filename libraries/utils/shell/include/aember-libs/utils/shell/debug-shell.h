/**
 * @file debug-shell.h
 * @author Arian Ajdari
 * @brief Debug shell on the console without delaying boot.
 *
 *        A listener thread watches the console. Pressing 'd' at any time
 *        starts /bin/sh on the console; when it exits, the listener waits
 *        for the next 'd'. Boot never waits for a key. A shell can also be
 *        opened right away (kernel parameter aember.shell=1, or as the
 *        emergency shell when the switch to the real root fails).
 *
 *        PID 1's reaper collects the shell, so the listener never waits for
 *        it itself: OnSpawn registers the pid (ChildSupervisor::AddChild)
 *        and IsAlive asks whether it is still tracked.
 *
 * @version 0.2
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <string>
#include <thread>

#include <sys/types.h>

namespace aember::utils::shell {

class DebugShell {
 public:
  /// Called with the shell's pid right after the fork.
  using OnSpawn = std::move_only_function<void(pid_t)>;
  /// True while the shell with this pid has not been reaped.
  using IsAlive = std::move_only_function<bool(pid_t)>;

  DebugShell();
  ~DebugShell();

  DebugShell(const DebugShell&) = delete;
  DebugShell& operator=(const DebugShell&) = delete;

  /// Start the console listener. With @p shell_now a shell opens at once.
  /// @p console is the console device (tests may pass another tty).
  void Start(OnSpawn on_spawn, IsAlive is_alive, bool shell_now,
             const std::string& console = "/dev/console");

  /// Stop the listener and restore the console. A running shell is left
  /// alone (shutdown kills it with everything else).
  void Stop();

  /// True if the kernel command line has aember.shell=1 (or =yes/=true).
  static bool RequestedOnCmdline(const std::string& path = "/proc/cmdline");

 private:
  void Listen(std::stop_token st, const std::string& console, bool shell_now);
  /// Fork /bin/sh on the console as session leader. Returns pid or -1.
  pid_t SpawnShell(const std::string& console);
  /// Wait until the shell is reaped or the listener is stopped.
  void WaitForShell(std::stop_token st, pid_t pid);

  OnSpawn on_spawn_;
  IsAlive is_alive_;
  int wake_pipe_[2] = {-1, -1};
  std::jthread thread_;
  aember::utils::logging::Logger log_;
};

}  // namespace aember::utils::shell
