/**
 * @file aember-init.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Top-level init system interface for Aember.
 */

#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/container-manager/container-manager.h>
#include <aember-libs/device-health/heartbeat.h>
#include <aember-libs/module-loader/module-loader.h>
#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/network-manager/network-manager.h>
#include <aember-libs/process-manager/process-manager.h>
#include <aember-libs/root-manager/root-manager.h>
#include <aember-libs/service-manager/dependency-resolver.h>
#include <aember-libs/service-manager/service-manager.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/shell/debug-shell.h>
#include <aember-libs/utils/signal/signal.h>

#include <atomic>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace aember::aember_init {

class AemberInit {
 public:
  using DebugShell = aember::utils::shell::DebugShell;
  using ContainerState = aember::utils::container::ContainerState;
  using ProcessState = aember::process_manager::ProcessManager::ProcessState;
  using Logger = aember::utils::logging::Logger;

  /// Lightweight error type used throughout init — just a message string.
  struct Error {
    std::string message;
  };

  template <typename T = void>
  using Result = std::expected<T, Error>;

  explicit AemberInit(std::string_view logger_name);
  ~AemberInit();

  AemberInit(const AemberInit&) = delete;
  AemberInit& operator=(const AemberInit&) = delete;

  void StartInitramfs();
  void StartRoot();
  void Stop();

 private:
  // ---------------------------------------------------------------------------
  // Init phases — each returns Result<> so failures chain cleanly
  // ---------------------------------------------------------------------------

  Result<> InitMounts();
  Result<> InitNetwork();
  Result<> InitServiceStack();
  Result<> InitSignals();

  Result<> LoadAndRegisterProcesses(std::string_view path);
  Result<> LoadAndRegisterContainers(std::string_view path);

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

  void OnNetworkStatus(
      const aember::utils::network::ConnectivityStatus& status);

  void OnContainerStateChange(const std::string& name, ContainerState old_state,
                              ContainerState new_state);

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------

  std::atomic<bool> running_{false};
  std::mutex mtx_;
  std::condition_variable cv_;

  // ---------------------------------------------------------------------------
  // Subsystems — constructed in dependency order, destroyed in reverse
  // ---------------------------------------------------------------------------

  aember::utils::signal::SignalHandler signal_handler_;
  aember::child_supervisor::ChildSupervisor child_supervisor_;

  std::optional<aember::device_health::Heartbeat> heartbeat_;
  std::optional<DebugShell> debug_shell_;
  std::optional<aember::module_loader::ModuleLoader> module_loader_;

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;
  std::shared_ptr<aember::container_manager::ContainerManager>
      container_manager_;

  std::unique_ptr<aember::network::NetworkManager> network_manager_;
  std::unique_ptr<aember::root_manager::RootManager> root_manager_;

  // ---------------------------------------------------------------------------
  // Service stack — order matters: process_manager_ and dependency_resolver_
  // must outlive service_manager_
  // ---------------------------------------------------------------------------

  std::unique_ptr<aember::process_manager::ProcessManager> process_manager_;
  std::unique_ptr<aember::service_manager::DependencyResolver>
      dependency_resolver_;
  std::unique_ptr<aember::service_manager::ServiceManager> service_manager_;

  Logger log_;
};

}  // namespace aember::aember_init
