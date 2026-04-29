/**
 * @file service-manager.h
 * @author Arian Ajdari
 * @brief Library definition for ServiceManager in Aember
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/service-manager/service.h>
#include <aember-libs/utils/container/container-config.h>
#include <aember-libs/utils/container/container-state.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/service/service-config.h>
#include <aember-libs/utils/service/service-parser.h>
#include <aember-libs/utils/service/service-state.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace aember::service_manager {

class ServiceManager {
 public:
  using ServiceState = aember::utils::service::ServiceState;
  using ServiceConfig = aember::utils::service::ServiceConfig;
  using Logger = aember::utils::logging::Logger;

  using ContainerState = aember::utils::container::ContainerState;
  using ContainerConfig = aember::utils::container::ContainerConfig;

  using StateChangeCallback =
      std::function<void(const std::string&, ServiceState, ServiceState)>;

  // 🔥 Single callback for containers (start/stop driven externally)
  using ContainerStateCallback =
      std::function<bool(const std::string&, ContainerState)>;

  ServiceManager(aember::child_supervisor::ChildSupervisor& supervisor);
  ~ServiceManager();

  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

  // --------------------------
  // Service management
  // --------------------------

  bool AddService(const ServiceConfig& config);

  bool RemoveService(const std::string& name);

  bool StartService(const std::string& name);

  bool StopService(const std::string& name);

  bool RestartService(const std::string& name);

  // --------------------------
  // Container registration
  // --------------------------

  bool AddContainer(const ContainerConfig& config);

  std::vector<aember::utils::container::ContainerConfig> GetContainers() const;

  // --------------------------
  // Bulk operations
  // --------------------------

  void StartAll();

  void StopAll();

  // --------------------------
  // Service queries
  // --------------------------

  bool HasService(const std::string& name) const;

  ServiceState GetServiceState(const std::string& name) const;

  std::vector<std::string> GetServiceNames() const;

  std::shared_ptr<Service> GetService(const std::string& name) const;

  // --------------------------
  // Child process integration
  // --------------------------

  void HandleServiceExit(pid_t pid, int exit_code);

  // --------------------------
  // Callbacks
  // --------------------------

  void SetStateChangeCallback(StateChangeCallback callback);

  void SetContainerStateCallback(ContainerStateCallback callback);

  // --------------------------
  // Config loading
  // --------------------------

  std::vector<ServiceConfig> LoadServices(const std::string& name);

 private:
  // --------------------------
  // Internal helpers
  // --------------------------

  bool StartServiceInternal(const std::string& name, bool is_restart = false);

  bool StopServiceInternal(const std::string& name);

  bool StartServiceDependencies(const std::string& name);

  bool CheckDependencies(const std::string& name) const;

  void ScheduleRestart(const std::string& name);

  pid_t SpawnProcess(const ServiceConfig& config);

  void ChangeServiceState(const std::string& name, ServiceState new_state);

  // --------------------------
  // Members
  // --------------------------

  std::map<std::string, std::shared_ptr<Service>> services_;

  std::map<pid_t, std::string> pid_to_service_;

  std::map<std::string, aember::utils::container::ContainerConfig> containers_;

  mutable std::mutex mutex_;

  aember::child_supervisor::ChildSupervisor& child_supervisor_;

  StateChangeCallback state_change_callback_;

  ContainerStateCallback container_state_callback_;

  aember::utils::service::ServicesConfigParser parser_{};

  mutable Logger log_;
};

}  // namespace aember::service_manager
