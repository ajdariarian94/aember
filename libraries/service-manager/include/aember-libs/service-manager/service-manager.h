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
#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aember::service_manager {

/**
 * @brief Manages services: start, stop, restart, and monitor state.
 *
 * ServiceManager works with ChildSupervisor to track child processes and
 * provides callbacks for service state changes.
 */
class ServiceManager {
 public:
  using StateChangeCallback =
      std::function<void(const std::string&, ServiceState, ServiceState)>;

  /**
   * @brief Construct a ServiceManager using a ChildSupervisor.
   *
   * @param supervisor Reference to a ChildSupervisor to manage child processes
   */
  ServiceManager(aember::child_supervisor::ChildSupervisor& supervisor);

  /**
   * @brief Destructor.
   * Stops all managed services before destruction.
   */
  ~ServiceManager();

  // Non-copyable
  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

  // --------------------------
  // Service management
  // --------------------------

  /**
   * @brief Add a service configuration to the manager.
   *
   * @param config ServiceConfig to add
   * @return true if added successfully, false if service already exists
   */
  bool AddService(const ServiceConfig& config);

  /**
   * @brief Remove a service from the manager.
   *
   * @param name Name of the service
   * @return true if removed, false if service not found
   */
  bool RemoveService(const std::string& name);

  /**
   * @brief Start a service by name.
   *
   * @param name Service name
   * @return true if started successfully
   */
  bool StartService(const std::string& name);

  /**
   * @brief Stop a service by name.
   *
   * @param name Service name
   * @return true if stopped successfully
   */
  bool StopService(const std::string& name);

  /**
   * @brief Restart a service by name.
   *
   * @param name Service name
   * @return true if restarted successfully
   */
  bool RestartService(const std::string& name);

  // --------------------------
  // Bulk operations
  // --------------------------

  /**
   * @brief Start all services.
   */
  void StartAll();

  /**
   * @brief Stop all services.
   */
  void StopAll();

  // --------------------------
  // Service queries
  // --------------------------

  /**
   * @brief Check if a service exists by name.
   *
   * @param name Service name
   * @return true if service exists
   */
  bool HasService(const std::string& name) const;

  /**
   * @brief Get the current state of a service.
   *
   * @param name Service name
   * @return ServiceState Current state
   */
  ServiceState GetServiceState(const std::string& name) const;

  /**
   * @brief Get names of all managed services.
   *
   * @return std::vector<std::string> List of service names
   */
  std::vector<std::string> GetServiceNames() const;

  /**
   * @brief Get a shared pointer to a service by name.
   *
   * @param name Service name
   * @return std::shared_ptr<Service> Pointer to service or nullptr if not found
   */
  std::shared_ptr<Service> GetService(const std::string& name) const;

  // --------------------------
  // Child process integration
  // --------------------------

  /**
   * @brief Handle a service process exit (called by init on SIGCHLD).
   *
   * @param pid Process ID
   * @param exit_code Exit code of the child process
   */
  void HandleServiceExit(pid_t pid, int exit_code);

  // --------------------------
  // Callbacks
  // --------------------------

  /**
   * @brief Register a callback to be notified on service state changes.
   *
   * @param callback Function taking service name, old state, new state
   */
  void SetStateChangeCallback(StateChangeCallback callback);

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

  std::map<std::string, std::shared_ptr<Service>>
      services_;  ///< All managed services
  std::map<pid_t, std::string>
      pid_to_service_;        ///< Map from pid to service name
  mutable std::mutex mutex_;  ///< Protects services_ and pid_to_service_

  aember::child_supervisor::ChildSupervisor&
      child_supervisor_;  ///< Supervisor for child processes
  StateChangeCallback state_change_callback_;  ///< Callback for state changes

  mutable aember::utils::Logger log_;  ///< Logger instance
};

}  // namespace aember::service_manager
