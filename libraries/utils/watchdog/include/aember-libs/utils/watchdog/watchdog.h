/**
 * @file watchdog.h
 * @author Arian Ajdari
 * @brief Hardware watchdog (/dev/watchdog): reboots the board when PID 1
 *        stops feeding it.
 *
 *        Feeding is conditional: a thread feeds every interval, but only
 *        while the alive check passes (PID 1 asks: did my main loop and my
 *        reaper run recently?). A hung PID 1 therefore really resets the
 *        board, instead of a lone feeder thread keeping it alive. For a
 *        shutdown, SetUnconditional() keeps it fed while services stop and
 *        Disarm() turns it off (magic close) right before reboot(2).
 *
 * @version 0.1
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace aember::utils::watchdog {

struct Options {
  std::string device = "/dev/watchdog";
  /// Hardware timeout asked for (the driver may round it).
  std::chrono::seconds timeout{15};
};

/// Kernel command line: `aember.watchdog=0|off|no` disables it (nullopt),
/// `aember.watchdog=<seconds>` sets the timeout; default Options otherwise.
std::optional<Options> OptionsFromCmdline(
    const std::string& path = "/proc/cmdline");

class Watchdog {
 public:
  using AliveCheck = std::function<bool()>;

  /// Opens and arms the device; nullptr (logged) if there is none.
  static std::unique_ptr<Watchdog> Open(const Options& options);

  /// Leaves the watchdog armed (the board resets unless someone feeds it):
  /// only Disarm() turns it off.
  ~Watchdog();

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  /// Feed every timeout/3 while @p alive returns true.
  void Start(AliveCheck alive);
  /// Feed regardless of the alive check (shutdown in progress).
  void SetUnconditional(bool on) { unconditional_ = on; }
  /// Stop feeding and turn the watchdog off (magic close). False if the
  /// driver can't stop it (nowayout): then the reset follows in timeout.
  bool Disarm();

  /// One keepalive now.
  bool Feed();
  /// The timeout the driver uses.
  [[nodiscard]] std::chrono::seconds Timeout() const { return timeout_; }
  /// Keepalives sent so far (tests, logs).
  [[nodiscard]] unsigned long Feeds() const { return feeds_; }

 private:
  Watchdog(int fd, std::string device, std::chrono::seconds timeout);
  void Loop(std::stop_token st);

  int fd_ = -1;
  std::string device_;
  std::chrono::seconds timeout_;
  AliveCheck alive_;
  std::atomic<bool> unconditional_{false};
  std::atomic<unsigned long> feeds_{0};
  std::atomic<bool> starving_{false};
  std::jthread thread_;
  aember::utils::logging::Logger log_{"watchdog"};
};

}  // namespace aember::utils::watchdog
