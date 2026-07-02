/**
 * @file network-manager.h
 * @author Arian Ajdari
 * @brief NetworkManager — brings up network interfaces and monitors
 *        internet connectivity for PID1.
 * @version 0.3
 * @date 2026-06-12
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
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aember::network {

/**
 * Brings up configured network interfaces (DHCP or static, via netlink
 * with udhcpc fallback), creates bridge interfaces for LXC containers,
 * and periodically monitors internet connectivity via TCP probe.
 *
 * Uses std::jthread for the monitor loop — no manual running_ flag or
 * condition variable needed.
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

  /// move_only_function — callback is never copied.
  using StatusCallback =
      std::move_only_function<void(const ConnectivityStatus&)>;

  explicit NetworkManager(const nlohmann::json& config,
                          StatusCallback on_status = nullptr);
  ~NetworkManager();

  NetworkManager(const NetworkManager&) = delete;
  NetworkManager& operator=(const NetworkManager&) = delete;

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  /** Brings up interfaces, creates bridges, starts connectivity monitor. */
  void Start();

  /** Stops the connectivity monitor thread. */
  void Stop();

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] ConnectivityStatus GetStatus() const;
  [[nodiscard]] bool IsOnline() const;
  [[nodiscard]] NetworkInfo GetNetworkInfo();

  /** Blocks until internet connectivity is confirmed or timeout expires. */
  bool WaitForConnectivity(
      std::chrono::milliseconds timeout = std::chrono::seconds(60));

 private:
  // ---------------------------------------------------------------------------
  // Config parsing
  // ---------------------------------------------------------------------------

  void ParseConfig(const nlohmann::json& config);

  // ---------------------------------------------------------------------------
  // Interface bring-up
  // ---------------------------------------------------------------------------

  void BringUpInterfaces();
  bool BringUpInterface(const InterfaceConfig& iface);
  bool NetlinkSetInterfaceUp(std::string_view iface_name);
  bool NetlinkSetStaticAddress(const InterfaceConfig& iface);
  bool RunUdhcpc(const InterfaceConfig& iface);
  bool FallbackIpCommand(const std::vector<std::string>& args);
  void WriteResolvConf(const InterfaceConfig& iface);

  // ---------------------------------------------------------------------------
  // Bridge management
  // ---------------------------------------------------------------------------

  void CreateBridges();
  bool CreateBridge(const BridgeConfig& bridge);
  bool InterfaceExists(std::string_view name);

  // ---------------------------------------------------------------------------
  // Connectivity monitor
  // ---------------------------------------------------------------------------

  void MonitorLoop(std::stop_token stop_token);
  int PingOnce(std::string_view target_ip, int timeout_ms = 2000);
  int Ping(std::string_view target_ip);

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  [[nodiscard]] std::string FirstUpInterface() const;
  void UpdateStatus(bool online, int rtt_ms);
  int GetInterfaceIndex(std::string_view iface_name);

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  NetworkConfig config_;
  std::vector<InterfaceState> iface_states_;

  StatusCallback on_status_;

  ConnectivityStatus status_;
  mutable std::mutex status_mutex_;

  // jthread carries its own stop_token — no running_ flag or cv needed.
  std::jthread monitor_thread_;

  mutable Logger log_{"network-manager"};
};

}  // namespace aember::network
