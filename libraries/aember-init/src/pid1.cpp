/**
 * @file pid1.cpp
 * @author Arian Ajdari
 * @brief Last-resort handling that keeps PID 1 alive.
 * @version 0.1
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/aember-init/pid1.h>

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include <pthread.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aember::aember_init::pid1 {

namespace {

constexpr unsigned kTermGraceSeconds = 2;

/// Log to the console directly and to spdlog if it still works.
void Critical(const std::string& msg) {
  std::fprintf(stderr, "aember-init: %s\n", msg.c_str());
  std::fflush(stderr);
  try {
    spdlog::critical("{}", msg);
    spdlog::apply_all([](const auto& l) { l->flush(); });
  } catch (...) {}
}

bool IsMainThread() {
  return static_cast<pid_t>(::syscall(SYS_gettid)) == ::getpid();
}

void BlockAllSignals() {
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, nullptr);
}

sigset_t InitSignalSet() {
  sigset_t set;
  sigemptyset(&set);
  for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGCHLD}) {
    sigaddset(&set, sig);
  }
  return set;
}

}  // namespace

void BlockInitSignals() {
  const sigset_t set = InitSignalSet();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  // A blocked mask survives exec, so services would start with SIGTERM
  // blocked. Unblock in every fork() child (ProcessManager, modprobe, ip,
  // debug shell). Must be async-signal-safe: sigprocmask only.
  pthread_atfork(nullptr, nullptr, [] {
    const sigset_t s = InitSignalSet();
    sigprocmask(SIG_UNBLOCK, &s, nullptr);
  });
}

std::string_view ToString(ShutdownAction action) {
  switch (action) {
    case ShutdownAction::kNone:
      return "none";
    case ShutdownAction::kReboot:
      return "reboot";
    case ShutdownAction::kPowerOff:
      return "poweroff";
    case ShutdownAction::kHalt:
      return "halt";
  }
  return "unknown";
}

void InstallTerminateHandler() {
  std::set_terminate([] {
    std::string what = "unknown exception";
    if (const auto ep = std::current_exception()) {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) { what = e.what(); } catch (...) {
      }
    } else {
      what = "std::terminate without an active exception";
    }

    if (::getpid() != 1) {
      Critical("terminate: " + what);
      std::abort();
    }

    if (IsMainThread()) {
      Critical("uncaught exception in PID 1 main thread: " + what +
               "; reaping zombies from now on");
      ReapForever();
    }

    // Park only the failing thread; the others keep supervising.
    Critical("uncaught exception in a PID 1 thread: " + what +
             "; thread parked, init continues");
    BlockAllSignals();
    for (;;) { ::pause(); }
  });
}

void ReapForever() {
  BlockAllSignals();
  for (;;) {
    int status = 0;
    if (::waitpid(-1, &status, 0) < 0 && errno == ECHILD) { ::sleep(1); }
  }
}

void Finish(ShutdownAction action) {
  if (action == ShutdownAction::kNone) {
    Critical(
        "aember-init stopped without a shutdown request; PID 1 must "
        "not exit, reaping zombies from now on");
    ReapForever();
  }

  Critical(std::string{"shutting down: "} + std::string{ToString(action)});

  // Everything but PID 1 (and kernel threads) gets SIGTERM, then SIGKILL.
  ::kill(-1, SIGTERM);
  for (unsigned i = 0; i < kTermGraceSeconds * 10; ++i) {
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {}
    ::usleep(100'000);
  }
  ::kill(-1, SIGKILL);
  while (::waitpid(-1, nullptr, WNOHANG) > 0) {}

  ::sync();
  ::mount(nullptr, "/", nullptr, MS_REMOUNT | MS_RDONLY, nullptr);
  ::sync();

  const int cmd = action == ShutdownAction::kPowerOff ? RB_POWER_OFF
                  : action == ShutdownAction::kHalt   ? RB_HALT_SYSTEM
                                                      : RB_AUTOBOOT;
  ::reboot(cmd);

  Critical("reboot(2) failed: errno=" + std::to_string(errno) +
           "; reaping zombies from now on");
  ReapForever();
}

}  // namespace aember::aember_init::pid1
