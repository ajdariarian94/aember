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
#include <aember-services/netd/connectivity-status.h>
#include <aember-services/netd/dhcp-client.h>
#include <aember-services/netd/interface-config.h>
#include <aember-services/netd/interface-state.h>
#include <aember-services/netd/ip-mode.h>
#include <aember-services/netd/network-config.h>
#include <aember-services/netd/network-info.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aember::netd {

/**
 * Brings up configured network interfaces (DHCP or static, via netlink
 * with the built-in DHCP client) and periodically monitors internet
 * connectivity via TCP probe. (Container bridges are aember-containerd's.)
 *
 * Uses std::jthread for the monitor loop — no manual running_ flag or
 * condition variable needed.
 */
class NetworkManager {
 public:
  using ConnectivityStatus = aember::netd::ConnectivityStatus;
  using NetworkInfo = aember::netd::NetworkInfo;
  using NetworkConfig = aember::netd::NetworkConfig;
  using InterfaceConfig = aember::netd::InterfaceConfig;
  using InterfaceState = aember::netd::InterfaceState;
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

  /** Brings up interfaces, starts the connectivity monitor. */
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

  /// Keep the DHCP leases alive: renew after T1 (unicast), rebind after
  /// T2 (broadcast), start over on a NAK or at expiry. Call it regularly
  /// (netd: every status report); each call does at most one exchange per
  /// interface, so it returns within a few seconds.
  void MaintainLeases();

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
  /// Address (+ default route, DNS) from network.json, via rtnetlink.
  bool ConfigureStatic(const InterfaceConfig& iface, int ifindex);
  /// Address, default route and DNS from a DHCP lease (built-in client).
  bool ConfigureDhcp(const InterfaceConfig& iface, int ifindex,
                     int attempts = -1);
  /// Address, default route and DNS from @p lease; replaces @p previous
  /// when the server handed out another address. Remembers the lease.
  bool ApplyLease(const InterfaceConfig& iface, int ifindex,
                  const dhcp::Lease& lease,
                  std::optional<dhcp::Lease> previous = std::nullopt);
  /// A DHCP interface that already had an address (netd restarted): ask
  /// the server about it to learn the lease, or start over on a NAK.
  void AdoptAddress(const InterfaceConfig& iface, int ifindex, in_addr address);
  /// Write /etc/resolv.conf (8.8.8.8 + 1.1.1.1 if @p dns is empty).
  void WriteResolvConf(const std::vector<std::string>& dns);

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

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  NetworkConfig config_;
  std::vector<InterfaceState> iface_states_;

  struct DhcpState {
    InterfaceConfig iface;
    int ifindex{-1};
    dhcp::Lease lease;
    dhcp::LeaseTimer timer;
    std::chrono::steady_clock::time_point next_try{};
  };
  /// Leases of the DHCP interfaces (only touched by Start and
  /// MaintainLeases, i.e. netd's main thread).
  std::map<std::string, DhcpState> leases_;

  StatusCallback on_status_;

  ConnectivityStatus status_;
  mutable std::mutex status_mutex_;

  // jthread carries its own stop_token — no running_ flag or cv needed.
  std::jthread monitor_thread_;

  mutable Logger log_{"network-manager"};
};

}  // namespace aember::netd
