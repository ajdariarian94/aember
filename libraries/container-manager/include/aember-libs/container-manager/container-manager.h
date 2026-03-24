/**
 * @file container_manager.h
 * @author Arian Ajdari
 * @brief Library definition for ContainerManager - manages LXC container
 *        lifecycle for PID1.
 * @version 0.1
 * @date 2026-03-23
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct lxc_container;

namespace aember::container_manager {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class ContainerState {
  kStopped,
  kStarting,
  kRunning,
  kStopping,
  kFailed,
};

std::string ContainerStateToString(ContainerState state);

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct ContainerConfig {
  std::string name;
  std::string rootfs;

  std::string config_path{"/etc/lxc/x64-linux.conf"};
  std::string lxc_path{"/var/lib/lxc"};

  std::vector<std::string> args;

  bool auto_start{true};
  bool restart_on_crash{true};

  int restart_delay_ms{2000};
  int max_restarts{5};
};

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

struct ContainerEntry {
  ContainerConfig config;

  ContainerState state{ContainerState::kStopped};
  int restart_count{0};

  int64_t started_at_ms{0};
  int64_t stopped_at_ms{0};

  int64_t next_restart_at_ms{0};  // 🔥 non-blocking restart scheduling

  lxc_container* lxc{nullptr};
};

// ---------------------------------------------------------------------------
// ContainerManager
// ---------------------------------------------------------------------------

class ContainerManager {
 public:
  using StateCallback =
      std::function<void(const std::string&, ContainerState, ContainerState)>;

  explicit ContainerManager(const nlohmann::json& config,
                            StateCallback cb = nullptr);
  ~ContainerManager();

  ContainerManager(const ContainerManager&) = delete;
  ContainerManager& operator=(const ContainerManager&) = delete;

  // Lifecycle
  void Start();
  void Stop();

  bool StartContainer(const std::string& name);
  bool StopContainer(const std::string& name);
  bool DestroyContainer(const std::string& name);

  ContainerState GetContainerState(const std::string& name) const;
  bool IsRunning(const std::string& name) const;

 private:
  // Config
  void ParseConfig(const nlohmann::json& config);

  // LXC lifecycle
  bool Create(ContainerEntry& e);
  bool Start(ContainerEntry& e);
  bool Stop(ContainerEntry& e);
  void Release(ContainerEntry& e);

  // Monitor
  void MonitorLoop();
  void Tick(ContainerEntry& e);

  // Helpers
  void SetState(ContainerEntry& e, ContainerState s);
  static int64_t NowMs();

  ContainerEntry* Find(const std::string& name);
  const ContainerEntry* Find(const std::string& name) const;

 private:
  std::vector<ContainerEntry> containers_;

  StateCallback callback_;

  std::atomic_bool running_{false};
  std::thread monitor_thread_;

  std::condition_variable cv_;
  std::mutex cv_mutex_;
  mutable std::mutex containers_mutex_;

  std::chrono::milliseconds interval_{2000};

  aember::utils::Logger log_{"container-manager"};
};

}  // namespace aember::container_manager
