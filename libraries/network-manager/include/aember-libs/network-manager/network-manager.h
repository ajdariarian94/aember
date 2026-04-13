/**
 * @file network_manager.h
 * @author Arian Ajdari
 * @brief Library definition for NetworkManager - brings up network interfaces
 *        and monitors internet connectivity for PID1.
 * @version 0.2
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/network/bridge-config.h>
#include <aember-libs/utils/network/connectivity-status.h>
#include <aember-libs/utils/network/interface-config.h>
#include <aember-libs/utils/network/interface-state.h>
#include <aember-libs/utils/network/ip-mode.h>
#include <aember-libs/utils/network/network-config.h>
#include <aember-libs/utils/network/network-info.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aember::network {

// ---------------------------------------------------------------------------
// NetworkManager
// ---------------------------------------------------------------------------

/**
 * @class NetworkManager
 * @brief Brings up configured network interfaces (DHCP or static, via netlink
 *        with udhcpc fallback), creates bridge interfaces for LXC containers,
 *        and periodically monitors internet connectivity via TCP probe.
 *
 * Usage:
 * @code
 *   NetworkManager net(config_json, [](const ConnectivityStatus& s) {
 *     if (!s.online) spdlog::warn("Internet lost!");
 *   });
 *   net.Start();
 * @endcode
 */
class NetworkManager {
 public:
  using ConnectivityStatus = aember::utils::network::ConnectivityStatus;
  using NetworkInfo = aember::utils::network::NetworkInfo;
  using NetworkConfig = aember::utils::network::NetworkConfig;
  using InterfaceConfig = aember::utils::network::InterfaceConfig;
  using BridgeConfig = aember::utils::network::BridgeConfig;
  using InterfaceState = aember::utils::network::InterfaceState;
  using Logger = aember::utils::logging::Logger;

  explicit NetworkManager(
      const nlohmann::json& config,
      std::function<void(const ConnectivityStatus&)> on_status = nullptr);

  ~NetworkManager();

  NetworkManager(const NetworkManager&) = delete;
  NetworkManager& operator=(const NetworkManager&) = delete;

  /**
   * @brief Brings up interfaces, creates bridges, starts connectivity monitor.
   * @throws std::runtime_error if a required interface fails to come up.
   */
  void Start();

  /**
   * @brief Stops the connectivity monitor thread.
   */
  void Stop();

  /**
   * @brief Returns the last known connectivity status.
   */
  ConnectivityStatus GetStatus() const;

  /**
   * @brief Returns true if internet is reachable.
   */
  bool IsOnline() const;

  /**
   * @brief Returns a full snapshot of the active interface.
   */
  NetworkInfo GetNetworkInfo();

  /**
   * @brief Blocks until internet connectivity is confirmed or timeout expires.
   */
  bool WaitForConnectivity(
      std::chrono::milliseconds timeout = std::chrono::seconds(60));

 private:
  // --- Config parsing -------------------------------------------------------
  void ParseConfig(const nlohmann::json& config);

  // --- Interface bring-up ---------------------------------------------------
  void BringUpInterfaces();
  bool BringUpInterface(const InterfaceConfig& iface);
  bool NetlinkSetInterfaceUp(const std::string& iface_name);
  bool NetlinkSetStaticAddress(const InterfaceConfig& iface);
  bool RunUdhcpc(const InterfaceConfig& iface);
  bool FallbackIpCommand(const std::vector<std::string>& args);
  void WriteResolvConf(const InterfaceConfig& iface);

  // --- Bridge management ----------------------------------------------------

  /**
   * @brief Creates all bridges configured in the JSON config.
   */
  void CreateBridges();

  /**
   * @brief Creates a single bridge and assigns its IP address.
   *        Uses ip link add type bridge + ip addr add + ip link set up.
   *        Skips gracefully if bridge already exists.
   * @return true on success.
   */
  bool CreateBridge(const BridgeConfig& bridge);

  /**
   * @brief Returns true if the named interface already exists in the kernel.
   */
  bool InterfaceExists(const std::string& name);

  // --- Connectivity monitor -------------------------------------------------
  void MonitorLoop();
  int PingOnce(const std::string& target_ip, int timeout_ms = 2000);
  int Ping(const std::string& target_ip);

  // --- Helpers --------------------------------------------------------------
  std::string FirstUpInterface() const;
  void UpdateStatus(bool online, int rtt_ms);
  int GetInterfaceIndex(const std::string& iface_name);

  // --- Members --------------------------------------------------------------
  NetworkConfig config_;
  std::vector<InterfaceState> iface_states_;

  std::function<void(const ConnectivityStatus&)> on_status_;

  ConnectivityStatus status_;
  mutable std::mutex status_mutex_;

  std::atomic_bool running_{false};
  std::thread monitor_thread_;
  std::condition_variable cv_;
  std::mutex cv_mutex_;

  Logger log_;
};

}  // namespace aember::network
