#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/device-health/heartbeat.h>
#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/service-manager/service-manager.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/signal/signal.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace aember::aember_init {

class AemberInit {
 public:
  AemberInit();
  ~AemberInit();

  void Start();
  void Stop();

 private:
  void RunLoop();
  void HeartbeatCallback(const nlohmann::json& heartbeat_payload);
  void OnServiceStateChangeCallback(
      const std::string& name, aember::service_manager::ServiceState old_state,
      aember::service_manager::ServiceState new_state);

  std::atomic<bool> running_;
  std::mutex mtx_;
  std::condition_variable cv_;

  aember::utils::SignalHandler signal_handler_;
  aember::child_supervisor::ChildSupervisor child_supervisor_;
  std::optional<aember::device_health::Heartbeat> heartbeat_;
  std::optional<aember::mount_manager::MountManager> mount_manager_;
  std::unique_ptr<aember::service_manager::ServiceManager> service_manager_;

  aember::utils::Logger log_;
};

}  // namespace aember::aember_init
