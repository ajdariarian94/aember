/**
 * @file pid1.h
 * @author Arian Ajdari
 * @brief Last-resort handling that keeps PID 1 alive.
 *
 *        If PID 1 exits or dies, the kernel panics. Everything here exists so
 *        that never happens: a terminate handler for uncaught exceptions, and
 *        Finish(), which ends main() with a reboot/poweroff/halt when one was
 *        requested and otherwise keeps reaping zombies forever.
 *
 * @version 0.1
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace aember::aember_init::pid1 {

/// What PID 1 does when it stops. Requested with BusyBox-compatible signals:
/// SIGTERM/SIGINT reboot, SIGUSR2 power off, SIGUSR1 halt.
enum class ShutdownAction : std::uint8_t {
  kNone,  ///< no request: PID 1 must not exit, it keeps reaping
  kReboot,
  kPowerOff,
  kHalt,
};

std::string_view ToString(ShutdownAction action);

/// Block the signals PID 1 handles through signalfd (SIGTERM, SIGINT,
/// SIGHUP, SIGUSR1, SIGUSR2, SIGCHLD). Must run first in main(), before any
/// thread exists: a process-directed signal goes to any thread that doesn't
/// block it, and a thread started before SignalHandler::Start() would take
/// it with the default action, which kills init (kernel panic). Children
/// created with fork() get the signals unblocked again (pthread_atfork).
void BlockInitSignals();

/// std::set_terminate handler: logs the uncaught exception, then in PID 1
/// parks the failing thread (other threads keep supervising) or, if it is the
/// main thread, reaps forever. Outside PID 1 it aborts as usual.
void InstallTerminateHandler();

/// Reap every child forever (waitpid(-1) blocking). Never returns.
[[noreturn]] void ReapForever();

/// End PID 1. kNone: log and ReapForever(). Otherwise SIGTERM then SIGKILL
/// to every other process, sync, remount / read-only and reboot(2); if that
/// fails, ReapForever(). Never returns.
[[noreturn]] void Finish(ShutdownAction action);

}  // namespace aember::aember_init::pid1
