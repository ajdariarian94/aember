/**
 * @file watchdog.cpp
 * @author Arian Ajdari
 * @brief Hardware watchdog implementation (Linux watchdog API).
 * @version 0.1
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/watchdog/watchdog.h>

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace aember::utils::watchdog {

std::optional<Options> OptionsFromCmdline(const std::string& path) {
  Options options;
  std::ifstream file{path};
  std::string token;
  constexpr std::string_view kKey = "aember.watchdog=";
  while (file >> token) {
    if (!token.starts_with(kKey)) continue;
    const auto value = token.substr(kKey.size());
    if (value == "0" || value == "off" || value == "no" || value == "false") {
      return std::nullopt;
    }
    try {
      std::size_t used = 0;
      const int seconds = std::stoi(value, &used);
      if (used == value.size() && seconds > 0) {
        options.timeout = std::chrono::seconds{seconds};
      }
    } catch (const std::exception&) {}  // keep the default
  }
  return options;
}

std::unique_ptr<Watchdog> Watchdog::Open(const Options& options) {
  aember::utils::logging::Logger log{"watchdog"};
  const int fd = ::open(options.device.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    log.info("No watchdog ({}: {})", options.device, std::strerror(errno));
    return nullptr;
  }
  // Opening armed it. Ask for our timeout; keep the driver's if it refuses.
  int timeout = static_cast<int>(options.timeout.count());
  if (::ioctl(fd, WDIOC_SETTIMEOUT, &timeout) != 0) {
    timeout = 0;
    if (::ioctl(fd, WDIOC_GETTIMEOUT, &timeout) != 0 || timeout <= 0) {
      timeout = static_cast<int>(options.timeout.count());
    }
  }
  watchdog_info info{};
  std::string identity;
  if (::ioctl(fd, WDIOC_GETSUPPORT, &info) == 0) {
    const auto* id = reinterpret_cast<const char*>(info.identity);
    identity = " (" + std::string{id, strnlen(id, sizeof(info.identity))} + ")";
  }
  log.info(
      "Watchdog {} armed: {} s timeout{}", options.device, timeout, identity);
  return std::unique_ptr<Watchdog>(
      new Watchdog(fd, options.device, std::chrono::seconds{timeout}));
}

Watchdog::Watchdog(int fd, std::string device, std::chrono::seconds timeout)
    : fd_(fd), device_(std::move(device)), timeout_(timeout) {}

Watchdog::~Watchdog() {
  thread_ = {};  // stop feeding
  // Still armed: closing without the magic 'V' leaves it running.
  if (fd_ >= 0) ::close(fd_);
}

bool Watchdog::Feed() {
  if (fd_ < 0) return false;
  // Writing any byte is a keepalive (works for every driver; the ioctl
  // WDIOC_KEEPALIVE is optional).
  const char byte = '\0';
  if (::write(fd_, &byte, 1) != 1) return false;
  ++feeds_;
  return true;
}

void Watchdog::Start(AliveCheck alive) {
  if (fd_ < 0 || thread_.joinable()) return;
  alive_ = std::move(alive);
  Feed();
  thread_ = std::jthread{[this](std::stop_token st) { Loop(std::move(st)); }};
}

void Watchdog::Loop(std::stop_token st) {
  const auto interval = std::max<std::chrono::milliseconds>(
      std::chrono::milliseconds{200}, timeout_ / 3);
  std::mutex m;
  std::condition_variable_any cv;
  std::unique_lock lock{m};
  while (!st.stop_requested()) {
    cv.wait_for(lock, st, interval, [] { return false; });
    if (st.stop_requested()) break;
    bool ok = unconditional_.load();
    if (!ok) {
      try {
        ok = !alive_ || alive_();
      } catch (...) { ok = false; }
    }
    if (ok) {
      if (starving_.exchange(false)) log_.warn("PID 1 alive again, feeding");
      Feed();
    } else if (!starving_.exchange(true)) {
      log_.critical("PID 1 looks hung: not feeding the watchdog, reset in {} s",
                    timeout_.count());
    }
  }
}

bool Watchdog::Disarm() {
  thread_ = {};  // joins: no feed after the magic close
  if (fd_ < 0) return true;
  const char magic = 'V';
  const bool ok = ::write(fd_, &magic, 1) == 1;
  ::close(fd_);
  fd_ = -1;
  log_.info("Watchdog {} disarmed{}",
            device_,
            ok ? "" : " (magic close failed; it may still reset)");
  return ok;
}

}  // namespace aember::utils::watchdog
