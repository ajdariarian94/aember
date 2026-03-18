/**
 * @file network_manager.h
 * @author Arian Ajdari
 * @brief Library definition for NetworkManager - brings up network interfaces
 *        and monitors internet connectivity for PID1.
 * @version 0.1
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
  std::string ping_target{
      "8.8.8.8"};  ///< ICMP ping target for connectivity check
  std::chrono::milliseconds ping_interval{
      std::chrono::seconds(10)};  ///< How often to ping
  int ping_retries{3};            ///< Ping attempts before declaring offline
  std::chrono::milliseconds dhcp_timeout{
      std::chrono::seconds(30)};  ///< DHCP acquisition timeout
  int dhcp_retries{3};            ///< DHCP attempts before fallback / failure
};

/**
 * @brief Connectivity status reported via the status callback.
 */
struct ConnectivityStatus {
  bool online{false};       ///< True if ICMP ping succeeded
  std::string interface;    ///< Interface used for the ping
  int rtt_ms{-1};           ///< Round-trip time in milliseconds, -1 if offline
  int64_t timestamp_ms{0};  ///< Epoch milliseconds of last check
};

/**
 * @brief Snapshot of the current network state — IP, gateway, DNS, MAC,
 * connectivity. Returned by GetNetworkInfo().
 */
struct NetworkInfo {
  std::string interface;                 ///< e.g. "eth0"
  std::string ip_address;                ///< e.g. "10.0.2.15"
  std::string netmask;                   ///< e.g. "255.255.255.0"
  int prefix_len{0};                     ///< e.g. 24
  std::string broadcast;                 ///< e.g. "10.0.2.255"
  std::string gateway;                   ///< e.g. "10.0.2.2"
  std::string mac_address;               ///< e.g. "52:54:00:12:34:56"
  std::vector<std::string> dns_servers;  ///< from /etc/resolv.conf
  bool online{false};                    ///< last known connectivity
  int rtt_ms{-1};                        ///< last TCP probe RTT
};

// ---------------------------------------------------------------------------
// NetworkManager
// ---------------------------------------------------------------------------

/**
 * @class NetworkManager
 * @brief Brings up configured network interfaces (DHCP or static, via netlink
 *        with udhcpc fallback) and periodically monitors internet connectivity
 *        via ICMP echo requests.
 *
 * Usage:
 * @code
 *   NetworkManager net(config_json, [](const ConnectivityStatus& s) {
 *     if (!s.online) spdlog::warn("Internet lost!");
 *   });
 *   net.Start();
 *   // ... later ...
 *   net.Stop();
 * @endcode
 */
class NetworkManager {
 public:
  /**
   * @brief Constructs a NetworkManager.
   * @param config        Parsed JSON config (see NetworkConfig fields).
   * @param on_status     Optional callback invoked on every connectivity check.
   */
  explicit NetworkManager(
      const nlohmann::json& config,
      std::function<void(const ConnectivityStatus&)> on_status = nullptr);

  ~NetworkManager();

  // Non-copyable, non-movable (owns threads and sockets)
  NetworkManager(const NetworkManager&) = delete;
  NetworkManager& operator=(const NetworkManager&) = delete;

  /**
   * @brief Parses config, brings up all configured interfaces, starts
   *        the background connectivity monitor thread.
   * @throws std::runtime_error if a required interface fails to come up.
   */
  void Start();

  /**
   * @brief Stops the connectivity monitor thread and tears down interfaces.
   */
  void Stop();

  /**
   * @brief Returns the last known connectivity status.
   */
  ConnectivityStatus GetStatus() const;

  /**
   * @brief Returns true if at least one interface is up and internet is
   * reachable.
   */
  bool IsOnline() const;

  /**
   * @brief Returns a full snapshot of the active interface: IP, netmask,
   * gateway, MAC, DNS servers, and last connectivity result. Reads live from
   * the kernel via ioctl + /proc/net/route + /etc/resolv.conf.
   */
  NetworkInfo GetNetworkInfo();

  /**
   * @brief Blocks until internet connectivity is confirmed or timeout expires.
   * @param timeout  Maximum time to wait.
   * @return true if online within timeout, false otherwise.
   */
  bool WaitForConnectivity(
      std::chrono::milliseconds timeout = std::chrono::seconds(60));

 private:
  // --- Config parsing -------------------------------------------------------
  void ParseConfig(const nlohmann::json& config);

  // --- Interface bring-up ---------------------------------------------------

  /**
   * @brief Brings up all interfaces according to their config.
   *        Blocks on required interfaces.
   */
  void BringUpInterfaces();

  /**
   * @brief Brings up a single interface (tries netlink first, falls back to
   *        busybox ip / udhcpc).
   */
  bool BringUpInterface(const InterfaceConfig& iface);

  /**
   * @brief Sets an interface administratively UP via netlink RTM_NEWLINK.
   * @return true on success.
   */
  bool NetlinkSetInterfaceUp(const std::string& iface_name);

  /**
   * @brief Assigns a static IP/prefix and default route via netlink.
   * @return true on success.
   */
  bool NetlinkSetStaticAddress(const InterfaceConfig& iface);

  /**
   * @brief Runs udhcpc to acquire a DHCP lease.
   *        Blocks until lease obtained or timeout/retries exhausted.
   * @return true if a lease was obtained.
   */
  bool RunUdhcpc(const InterfaceConfig& iface);

  /**
   * @brief Shells out to `ip` as a fallback for interface / route setup.
   */
  bool FallbackIpCommand(const std::vector<std::string>& args);

  /**
   * @brief Writes /etc/resolv.conf from the DNS servers in iface config.
   */
  void WriteResolvConf(const InterfaceConfig& iface);

  // --- Connectivity monitor -------------------------------------------------

  /**
   * @brief Background thread: periodically pings ping_target_.
   */
  void MonitorLoop();

  /**
   * @brief Sends a single ICMP echo request and waits for a reply.
   * @param target_ip  Dotted-decimal IPv4 address.
   * @param timeout_ms Milliseconds to wait for reply.
   * @return RTT in milliseconds, or -1 on failure.
   */
  int PingOnce(const std::string& target_ip, int timeout_ms = 2000);

  /**
   * @brief Runs PingOnce() up to ping_retries_ times.
   * @return RTT of first success, or -1 if all attempts fail.
   */
  int Ping(const std::string& target_ip);

  // --- Helpers --------------------------------------------------------------

  /**
   * @brief Returns the name of the first interface currently in kUp state.
   */
  std::string FirstUpInterface() const;

  /**
   * @brief Updates connectivity status and fires the callback if set.
   */
  void UpdateStatus(bool online, int rtt_ms);

  /**
   * @brief Resolves interface index by name via ioctl.
   */
  int GetInterfaceIndex(const std::string& iface_name);

  // --- Members --------------------------------------------------------------

  NetworkConfig config_;
  std::vector<InterfaceState>
      iface_states_;  ///< Parallel to config_.interfaces

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
