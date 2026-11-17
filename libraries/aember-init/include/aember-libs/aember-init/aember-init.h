/**
 * @file aember-init.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Top-level init system interface for Aember.
 */

#pragma once

#include <aember-libs/aember-init/pid1.h>
#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/device-health/aember-monitor.h>
#include <aember-libs/device-health/heartbeat.h>
#include <aember-libs/module-loader/module-loader.h>
#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/process-manager/process-manager.h>
#include <aember-libs/root-manager/root-manager.h>
#include <aember-libs/service-manager/dependency-resolver.h>
#include <aember-libs/service-manager/service-manager.h>
#include <aember-libs/utils/ipc/hub.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/shell/debug-shell.h>
#include <aember-libs/utils/signal/signal.h>

#include <atomic>
#include <condition_variable>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace aember::aember_init {

/// How AemberInit runs.
struct InitOptions {
  /// Running chrooted on a host (`aember app launch --chroot`): no kernel
  /// modules (they would go into the host kernel); everything else as on a
  /// device.
  bool chroot = false;
};

/// Names of the built-in daemons (services in ServiceManager).
inline constexpr std::string_view kNetd = "aember-netd";
inline constexpr std::string_view kContainerd = "aember-containerd";

class AemberInit {
 public:
  using DebugShell = aember::utils::shell::DebugShell;
  using ProcessState = aember::process_manager::ProcessManager::ProcessState;
  using Logger = aember::utils::logging::Logger;
  using ShutdownAction = pid1::ShutdownAction;

  /// Lightweight error type used throughout init — just a message string.
  struct Error {
    std::string message;
  };

  template <typename T = void>
  using Result = std::expected<T, Error>;

  using Options = InitOptions;

  explicit AemberInit(std::string_view logger_name, Options options = {});
  ~AemberInit();

  AemberInit(const AemberInit&) = delete;
  AemberInit& operator=(const AemberInit&) = delete;

  /// Both return only after a shutdown was requested (or if the pivot
  /// exec'd away). Exceptions from a phase are logged, never propagated.
  void StartInitramfs();
  void StartRoot();
  void Stop();

  /// Set by the shutdown signals; main() passes it to pid1::Finish().
  ShutdownAction RequestedShutdown() const { return shutdown_.load(); }

 private:
  // ---------------------------------------------------------------------------
  // Init phases — each returns Result<> so failures chain cleanly
  // ---------------------------------------------------------------------------

  Result<> InitMounts();
  Result<> InitServiceStack();
  /// `aember netd` and `aember containerd` as services (ipc, always
  /// restarted, containerd after netd).
  void RegisterDaemons();
  /// A message from a daemon: readiness, network status, container state.
  void OnDaemonMessage(const std::string& name, const nlohmann::json& message);
  Result<> InitSignals();

  Result<> LoadAndRegisterProcesses(std::string_view path);

  void StartReaperThread();
  void StopReaperThread();
  void StartDebugShell(bool shell_now);
  void ReapOnce();
  void RequestShutdown(ShutdownAction action);

  // ---------------------------------------------------------------------------
  // Run loop
  // ---------------------------------------------------------------------------

  void RunLoop();

  // ---------------------------------------------------------------------------
  // Callbacks — move_only_function since they're never copied
  // ---------------------------------------------------------------------------

  void OnHeartbeat(const nlohmann::json& payload);

  void OnServiceStateChange(const std::string& name, ProcessState old_state,
                            ProcessState new_state);

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------

  Options options_;
  std::atomic<bool> running_{false};
  std::atomic<ShutdownAction> shutdown_{ShutdownAction::kNone};
  std::mutex mtx_;
  std::condition_variable cv_;

  // The reaper thread is the only place that collects children. SIGCHLD
  // only wakes it; it also polls every second because SIGCHLD can be lost
  // (threads started before the signalfd don't block it).
  std::mutex reap_mtx_;
  std::condition_variable_any reap_cv_;
  bool reap_pending_ = false;

  // ---------------------------------------------------------------------------
  // Subsystems — constructed in dependency order, destroyed in reverse
  // ---------------------------------------------------------------------------

  aember::utils::signal::SignalHandler signal_handler_;
  aember::child_supervisor::ChildSupervisor child_supervisor_;

  std::optional<aember::device_health::Heartbeat> heartbeat_;
  std::optional<DebugShell> debug_shell_;
  std::optional<aember::module_loader::ModuleLoader> module_loader_;

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;

  std::unique_ptr<aember::root_manager::RootManager> root_manager_;

  std::unique_ptr<aember::device_health::AemberMonitor> monitor_;
  // ---------------------------------------------------------------------------
  // Service stack — order matters: process_manager_ and dependency_resolver_
  // must outlive service_manager_
  // ---------------------------------------------------------------------------

  std::unique_ptr<aember::process_manager::ProcessManager> process_manager_;
  std::unique_ptr<aember::service_manager::DependencyResolver>
      dependency_resolver_;
  std::unique_ptr<aember::service_manager::ServiceManager> service_manager_;

  std::jthread reaper_thread_;

  /// Daemon channels (netd, containerd) and what they last reported.
  aember::utils::ipc::Hub hub_;
  std::mutex status_mutex_;
  nlohmann::json network_status_;
  std::map<std::string, std::string> container_states_;

  Logger log_;
};

}  // namespace aember::aember_init
