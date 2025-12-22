/**
 * @file aember-init.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Top-level init system interface for Aember.
 *
 * Defines the AemberInit class, responsible for bootstrapping the system,
 * managing services, handling signals, supervising child processes, and
 * coordinating early system initialization.
 */

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

/**
 * @class AemberInit
 * @brief Top-level init system orchestrator.
 *
 * AemberInit acts as the system's init process (PID 1 or equivalent).
 * It is responsible for:
 *
 * - Mounting early filesystems
 * - Handling system signals
 * - Supervising child processes
 * - Managing service lifecycles
 * - Providing heartbeat and health reporting
 *
 * The Start() method initializes all subsystems and enters the main run loop.
 */
class AemberInit {
 public:
  /**
   * @brief Construct the init system.
   *
   * Construction does not start any subsystems. Initialization is deferred
   * to Start().
   */
  AemberInit();

  /**
   * @brief Destructor.
   *
   * Ensures Stop() is called and all owned subsystems are shut down cleanly.
   */
  ~AemberInit();

  /**
   * @brief Start the init system.
   *
   * Initializes logging, mounts early filesystems, installs signal handlers,
   * initializes service management, and enters the main run loop.
   *
   * This function blocks until Stop() is invoked.
   */
  void Start();

  /**
   * @brief Stop the init system.
   *
   * Gracefully shuts down all managed services and subsystems.
   * Safe to call multiple times.
   */
  void Stop();

 private:
  /**
   * @brief Main run loop for the init process.
   *
   * Keeps the init process alive and provides a place for periodic tasks
   * such as health checks or housekeeping.
   */
  void RunLoop();

  /**
   * @brief Callback invoked by the heartbeat subsystem.
   *
   * Receives periodic health and liveness information.
   *
   * @param heartbeat_payload JSON payload describing system health.
   */
  void HeartbeatCallback(const nlohmann::json& heartbeat_payload);

  /**
   * @brief Callback invoked when a service changes state.
   *
   * Registered with the ServiceManager to observe service lifecycle
   * transitions.
   *
   * @param name Name of the service.
   * @param old_state Previous service state.
   * @param new_state New service state.
   */
  void OnServiceStateChangeCallback(
      const std::string& name, aember::service_manager::ServiceState old_state,
      aember::service_manager::ServiceState new_state);

  /**
   * @brief Indicates whether the init system is currently running.
   *
   * Used to control the lifetime of the main run loop and to prevent
   * multiple starts or stops.
   */
  std::atomic<bool> running_;

  /**
   * @brief Mutex protecting the run loop condition variable.
   */
  std::mutex mtx_;

  /**
   * @brief Condition variable used to wake or block the run loop.
   */
  std::condition_variable cv_;

  /**
   * @brief Centralized signal handling utility.
   *
   * Used to register and dispatch handlers for signals such as SIGTERM,
   * SIGINT, SIGCHLD, and SIGHUP.
   */
  aember::utils::SignalHandler signal_handler_;

  /**
   * @brief Supervises all child processes.
   *
   * Tracks spawned child PIDs, assists with reaping, and provides
   * centralized child lifecycle management.
   */
  aember::child_supervisor::ChildSupervisor child_supervisor_;

  /**
   * @brief Optional heartbeat subsystem.
   *
   * Periodically emits health and liveness information while the
   * init system is running.
   */
  std::optional<aember::device_health::Heartbeat> heartbeat_;

  /**
   * @brief Optional mount manager.
   *
   * Responsible for mounting early filesystems required before
   * services are started.
   */
  std::optional<aember::mount_manager::MountManager> mount_manager_;

  /**
   * @brief Service manager instance.
   *
   * Owns and controls all configured system services, including
   * dependency management and restart policies.
   */
  std::unique_ptr<aember::service_manager::ServiceManager> service_manager_;

  /**
   * @brief Logger instance for the init system.
   */
  aember::utils::Logger log_;
};

}  // namespace aember::aember_init
