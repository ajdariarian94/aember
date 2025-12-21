#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/service-manager/service.h>
#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace aember::service_manager {

class ServiceManager {
 public:
  using StateChangeCallback =
      std::function<void(const std::string&, ServiceState, ServiceState)>;

  ServiceManager(aember::child_supervisor::ChildSupervisor& supervisor);
  ~ServiceManager();

  // Non-copyable
  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

  // Service management
  bool AddService(const ServiceConfig& config);
  bool RemoveService(const std::string& name);
  bool StartService(const std::string& name);
  bool StopService(const std::string& name);
  bool RestartService(const std::string& name);

  // Bulk operations
  void StartAll();
  void StopAll();

  // Service queries
  bool HasService(const std::string& name) const;
  ServiceState GetServiceState(const std::string& name) const;
  std::vector<std::string> GetServiceNames() const;
  std::shared_ptr<Service> GetService(const std::string& name) const;

  // Handle process exit (called by init when SIGCHLD received)
  void HandleServiceExit(pid_t pid, int exit_code);

  // State change notifications
  void SetStateChangeCallback(StateChangeCallback callback);

 private:
  bool StartServiceInternal(const std::string& name, bool is_restart = false);
  bool StopServiceInternal(const std::string& name);
  bool StartServiceDependencies(const std::string& name);
  bool CheckDependencies(const std::string& name) const;
  void ScheduleRestart(const std::string& name);
  pid_t SpawnProcess(const ServiceConfig& config);

  void ChangeServiceState(const std::string& name, ServiceState new_state);

  std::map<std::string, std::shared_ptr<Service>> services_;
  std::map<pid_t, std::string> pid_to_service_;
  mutable std::mutex mutex_;

  aember::child_supervisor::ChildSupervisor& child_supervisor_;
  StateChangeCallback state_change_callback_;

  mutable aember::utils::Logger log_;
};

}  // namespace aember::service_manager
