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
// Data structures
// ---------------------------------------------------------------------------

/**
 * @brief IP configuration mode for a network interface.
 */
enum class IpMode {
  kDhcp,    ///< Obtain address via DHCP (udhcpc)
  kStatic,  ///< Use a statically configured address
};

/**
 * @brief Configuration for a single network interface, loaded from JSON.
 */
struct InterfaceConfig {
  std::string name;            ///< e.g. "eth0"
  IpMode mode{IpMode::kDhcp};  ///< DHCP or static
  std::string address;         ///< Static IP in CIDR, e.g. "192.168.1.10/24"
  std::string gateway;         ///< Default gateway, e.g. "192.168.1.1"
  std::vector<std::string>
      dns_servers;       ///< DNS servers to write to resolv.conf
  bool required{false};  ///< If true, init blocks until this IF is up
};

/**
 * @brief Configuration for a Linux bridge interface, loaded from JSON.
 */
struct BridgeConfig {
  std::string name;     ///< Bridge name e.g. "lxcbr0"
  std::string address;  ///< IP in CIDR e.g. "10.0.3.1/24"
};

/**
 * @brief Runtime state of a network interface.
 */
enum class InterfaceState {
  kDown,        ///< Interface not yet brought up
  kBringingUp,  ///< Currently running udhcpc / applying static config
  kUp,          ///< Interface is up and has an address
  kFailed,      ///< Failed to come up after all retries
};

/**
 * @brief Full network configuration loaded from the config file.
 */
struct NetworkConfig {
  std::vector<InterfaceConfig> interfaces;
  std::vector<BridgeConfig> bridges;   ///< Bridge interfaces to create
  std::string ping_target{"8.8.8.8"};  ///< TCP probe target
  std::chrono::milliseconds ping_interval{std::chrono::seconds(10)};
  int ping_retries{3};
  std::chrono::milliseconds dhcp_timeout{std::chrono::seconds(30)};
  int dhcp_retries{3};
};

/**
 * @brief Connectivity status reported via the status callback.
 */
struct ConnectivityStatus {
  bool online{false};
  std::string interface;
  int rtt_ms{-1};
  int64_t timestamp_ms{0};
};

/**
 * @brief Snapshot of the current network state.
 */
struct NetworkInfo {
  std::string interface;
  std::string ip_address;
  std::string netmask;
  int prefix_len{0};
  std::string broadcast;
  std::string gateway;
  std::string mac_address;
  std::vector<std::string> dns_servers;
  bool online{false};
  int rtt_ms{-1};
};

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

  aember::utils::Logger log_;
};

}  // namespace aember::network
