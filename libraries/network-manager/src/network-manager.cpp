/**
 * @file network-manager.cpp
 * @author Arian Ajdari
 * @brief NetworkManager implementation.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/network-manager/dhcp-client.h>
#include <aember-libs/network-manager/netlink.h>
#include <aember-libs/network-manager/network-manager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace aember::network {

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace {

bool ParseCidr(const std::string& cidr, in_addr& addr, int& prefix_len) {
  const auto slash = cidr.find('/');
  if (slash == std::string::npos) { return false; }

  if (inet_pton(AF_INET, cidr.substr(0, slash).c_str(), &addr) != 1) {
    return false;
  }

  prefix_len = std::stoi(cidr.substr(slash + 1));
  return prefix_len >= 0 && prefix_len <= 32;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

NetworkManager::NetworkManager(const nlohmann::json& config,
                               StatusCallback on_status)
    : on_status_(std::move(on_status)) {
  ParseConfig(config);
  iface_states_.assign(config_.interfaces.size(), InterfaceState::kDown);
}

NetworkManager::~NetworkManager() {
  Stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void NetworkManager::Start() {
  log_.info("Starting NetworkManager");

  BringUpInterfaces();
  CreateBridges();

  // jthread passes stop_token automatically as first argument.
  monitor_thread_ =
      std::jthread{[this](std::stop_token st) { MonitorLoop(std::move(st)); }};

  log_.info("NetworkManager started — connectivity monitor running");
}

void NetworkManager::Stop() {
  monitor_thread_.request_stop();
  if (monitor_thread_.joinable()) { monitor_thread_.join(); }
  log_.info("NetworkManager stopped");
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

NetworkManager::ConnectivityStatus NetworkManager::GetStatus() const {
  std::lock_guard lock{status_mutex_};
  return status_;
}

bool NetworkManager::IsOnline() const {
  std::lock_guard lock{status_mutex_};
  return status_.online;
}

bool NetworkManager::WaitForConnectivity(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  log_.info("Waiting for connectivity (timeout {}ms)", timeout.count());

  while (std::chrono::steady_clock::now() < deadline) {
    if (IsOnline()) {
      log_.info("Connectivity confirmed");
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  }

  log_.warn("Timed out waiting for connectivity after {}ms", timeout.count());
  return false;
}

// ---------------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------------

void NetworkManager::ParseConfig(const nlohmann::json& config) {
  config_.ping_target = config.value("ping_target", "8.8.8.8");
  config_.ping_retries = config.value("ping_retries", 3);
  config_.ping_interval =
      std::chrono::milliseconds{config.value("ping_interval_ms", 10000)};
  config_.dhcp_timeout =
      std::chrono::milliseconds{config.value("dhcp_timeout_ms", 30000)};
  config_.dhcp_retries = config.value("dhcp_retries", 3);

  if (!config.contains("interfaces") || !config["interfaces"].is_array()) {
    throw std::runtime_error(
        "NetworkManager: config missing 'interfaces' array");
  }

  for (const auto& entry : config["interfaces"]) {
    InterfaceConfig iface;
    iface.name = entry.at("name").get<std::string>();
    iface.required = entry.value("required", false);

    const auto mode_str = entry.value("mode", "dhcp");
    if (mode_str == "static") {
      iface.mode = aember::utils::network::IpMode::kStatic;
      iface.address = entry.at("address").get<std::string>();
      iface.gateway = entry.value("gateway", "");
    } else {
      iface.mode = aember::utils::network::IpMode::kDhcp;
    }

    if (entry.contains("dns")) {
      std::ranges::transform(
          entry["dns"],
          std::back_inserter(iface.dns_servers),
          [](const auto& d) { return d.template get<std::string>(); });
    }

    log_.info("Parsed interface: {} mode={} required={}",
              iface.name,
              mode_str,
              iface.required);
    config_.interfaces.push_back(std::move(iface));
  }

  if (config.contains("bridges") && config["bridges"].is_array()) {
    for (const auto& entry : config["bridges"]) {
      BridgeConfig bridge;
      bridge.name = entry.at("name").get<std::string>();
      bridge.address = entry.at("address").get<std::string>();
      log_.info("Parsed bridge: {} address={}", bridge.name, bridge.address);
      config_.bridges.push_back(std::move(bridge));
    }
  }
}

// ---------------------------------------------------------------------------
// Interface bring-up
// ---------------------------------------------------------------------------

void NetworkManager::BringUpInterfaces() {
  log_.info("Bringing up {} interface(s)", config_.interfaces.size());

  // C++26 std::views::enumerate gives us index + value without a raw counter.
  for (auto [i, iface] : std::views::enumerate(config_.interfaces)) {
    iface_states_[i] = InterfaceState::kBringingUp;

    const bool ok = BringUpInterface(iface);
    iface_states_[i] = ok ? InterfaceState::kUp : InterfaceState::kFailed;

    if (!ok) {
      if (iface.required) {
        throw std::runtime_error(std::format(
            "NetworkManager: required interface '{}' failed to come up",
            iface.name));
      }
      log_.warn("Interface {} failed (not required, continuing)", iface.name);
    } else {
      log_.info("Interface {} is up", iface.name);
    }
  }
}

bool NetworkManager::BringUpInterface(const InterfaceConfig& iface) {
  const auto mode_str =
      iface.mode == aember::utils::network::IpMode::kDhcp ? "dhcp" : "static";
  log_.info("Bringing up interface: {} (mode={})", iface.name, mode_str);

  const int idx = netlink::InterfaceIndex(iface.name);
  if (idx < 0) {
    log_.error("{}: no such interface", iface.name);
    return false;
  }
  if (const int rc = netlink::SetLinkUp(idx); rc != 0) {
    log_.error("{}: could not set link up: {}", iface.name, strerror(-rc));
    return false;
  }

  // Already configured (a container runtime like Docker, the kernel's ip=
  // parameter, a previous aember): keep it rather than running DHCP over it.
  if (const auto existing = netlink::FirstIPv4Address(iface.name)) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &*existing, ip, sizeof(ip));
    log_.info("{}: keeping existing address {}", iface.name, ip);
    if (!iface.dns_servers.empty()) { WriteResolvConf(iface.dns_servers); }
    return true;
  }

  if (iface.mode == aember::utils::network::IpMode::kStatic) {
    return ConfigureStatic(iface, idx);
  }
  return ConfigureDhcp(iface, idx);
}

bool NetworkManager::ConfigureStatic(const InterfaceConfig& iface, int idx) {
  in_addr addr{};
  int prefix_len = 0;
  if (!ParseCidr(iface.address, addr, prefix_len)) {
    log_.error("{}: invalid CIDR '{}'", iface.name, iface.address);
    return false;
  }
  if (const int rc = netlink::AddAddress(idx, addr, prefix_len); rc != 0) {
    log_.error("{}: could not assign {}: {}",
               iface.name,
               iface.address,
               strerror(-rc));
    return false;
  }

  if (!iface.gateway.empty()) {
    in_addr gw{};
    if (inet_pton(AF_INET, iface.gateway.c_str(), &gw) != 1) {
      log_.warn("{}: invalid gateway '{}', no default route",
                iface.name,
                iface.gateway);
    } else if (const int rc = netlink::AddDefaultRoute(idx, gw); rc != 0) {
      log_.warn("{}: default route via {} failed: {}",
                iface.name,
                iface.gateway,
                strerror(-rc));
    }
  }
  WriteResolvConf(iface.dns_servers);
  return true;
}

bool NetworkManager::ConfigureDhcp(const InterfaceConfig& iface, int idx) {
  dhcp::DhcpClient client{iface.name};
  for (int attempt = 1; attempt <= config_.dhcp_retries; ++attempt) {
    log_.info(
        "{}: DHCP attempt {}/{}", iface.name, attempt, config_.dhcp_retries);

    auto lease = client.Acquire(config_.dhcp_timeout);
    if (!lease) {
      log_.warn("{}: {}", iface.name, lease.error());
      continue;
    }

    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &lease->address, ip, sizeof(ip));
    const int prefix = lease->PrefixLength();
    if (const int rc = netlink::AddAddress(idx, lease->address, prefix);
        rc != 0) {
      log_.error("{}: could not assign {}/{}: {}",
                 iface.name,
                 ip,
                 prefix,
                 strerror(-rc));
      continue;
    }

    std::string router = "none";
    if (lease->router) {
      char gw[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &*lease->router, gw, sizeof(gw));
      router = gw;
      if (const int rc = netlink::AddDefaultRoute(idx, *lease->router);
          rc != 0) {
        log_.warn("{}: default route via {} failed: {}",
                  iface.name,
                  router,
                  strerror(-rc));
      }
    }

    // Configured DNS servers win over the ones the lease offers.
    std::vector<std::string> dns = iface.dns_servers;
    if (dns.empty()) {
      for (const auto& d : lease->dns) {
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &d, buf, sizeof(buf));
        dns.emplace_back(buf);
      }
    }
    WriteResolvConf(dns);

    log_.info("{}: DHCP lease {}/{} router={} lease={}s",
              iface.name,
              ip,
              prefix,
              router,
              lease->lease_seconds);
    return true;
  }

  log_.error("{}: all DHCP attempts exhausted", iface.name);
  return false;
}

// ---------------------------------------------------------------------------
// Netlink: assign static address + default route
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// resolv.conf
// ---------------------------------------------------------------------------

void NetworkManager::WriteResolvConf(const std::vector<std::string>& dns) {
  const auto& servers =
      !dns.empty() ? dns : std::vector<std::string>{"8.8.8.8", "1.1.1.1"};

  std::ofstream f{"/etc/resolv.conf", std::ios::trunc};
  if (!f.is_open()) {
    log_.warn("Could not write /etc/resolv.conf");
    return;
  }

  f << "# Generated by aember NetworkManager\n";
  for (const auto& ns : servers) { f << "nameserver " << ns << '\n'; }

  log_.info("Wrote /etc/resolv.conf ({} nameserver(s))", servers.size());
}

// ---------------------------------------------------------------------------
// Bridge management
// ---------------------------------------------------------------------------

void NetworkManager::CreateBridges() {
  if (config_.bridges.empty()) { return; }

  log_.info("Creating {} bridge(s)", config_.bridges.size());

  std::ranges::for_each(config_.bridges, [this](const auto& b) {
    if (!CreateBridge(b)) {
      log_.warn("Failed to create bridge '{}', continuing", b.name);
    }
  });
}

bool NetworkManager::CreateBridge(const BridgeConfig& bridge) {
  log_.info("Creating bridge: {} address={}", bridge.name, bridge.address);

  if (const int rc = netlink::CreateBridge(bridge.name);
      rc != 0 && rc != -EEXIST) {
    log_.error("Failed to create bridge '{}': {}", bridge.name, strerror(-rc));
    return false;
  }

  const int idx = netlink::InterfaceIndex(bridge.name);
  if (idx < 0) {
    log_.error("Bridge '{}' missing after creation", bridge.name);
    return false;
  }

  if (!bridge.address.empty()) {
    in_addr addr{};
    int prefix_len = 0;
    if (!ParseCidr(bridge.address, addr, prefix_len)) {
      log_.error("Bridge '{}': invalid CIDR '{}'", bridge.name, bridge.address);
    } else if (const int rc = netlink::AddAddress(idx, addr, prefix_len);
               rc != 0) {
      log_.error("Failed to assign '{}' to bridge '{}': {}",
                 bridge.address,
                 bridge.name,
                 strerror(-rc));
    }
  }

  if (const int rc = netlink::SetLinkUp(idx); rc != 0) {
    log_.error(
        "Failed to bring up bridge '{}': {}", bridge.name, strerror(-rc));
    return false;
  }

  log_.info("Bridge '{}' up (address={})", bridge.name, bridge.address);
  return true;
}

bool NetworkManager::InterfaceExists(std::string_view name) {
  return GetInterfaceIndex(name) >= 0;
}

// ---------------------------------------------------------------------------
// Connectivity monitor
// ---------------------------------------------------------------------------

void NetworkManager::MonitorLoop(std::stop_token stop_token) {
  log_.info("Connectivity monitor started (target={}, interval={}ms)",
            config_.ping_target,
            config_.ping_interval.count());

  while (!stop_token.stop_requested()) {
    try {
      const int rtt = Ping(config_.ping_target);
      UpdateStatus(rtt >= 0, rtt);
    } catch (const std::exception& e) {
      log_.error("Connectivity check failed: {}", e.what());
    }
    std::this_thread::sleep_for(config_.ping_interval);
  }

  log_.info("Connectivity monitor stopped");
}

void NetworkManager::UpdateStatus(bool online, int rtt_ms) {
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  ConnectivityStatus s{
      .online = online,
      .rtt_ms = rtt_ms,
      .interface = FirstUpInterface(),
      .timestamp_ms = now_ms,
  };

  {
    std::lock_guard lock{status_mutex_};
    const bool was_online = status_.online;
    status_ = s;

    if (was_online && !online) {
      log_.warn("Internet connectivity LOST (last via {})", s.interface);
    } else if (!was_online && online) {
      log_.info("Internet connectivity RESTORED via {} (rtt={}ms)",
                s.interface,
                rtt_ms);
    }
  }

  if (on_status_) {
    try {
      on_status_(s);
    } catch (const std::exception& e) {
      log_.error("Status callback threw: {}", e.what());
    }
  }
}

// ---------------------------------------------------------------------------
// TCP probe
// ---------------------------------------------------------------------------

int NetworkManager::Ping(std::string_view target_ip) {
  for (int i = 0; i < config_.ping_retries; ++i) {
    if (const int rtt = PingOnce(target_ip); rtt >= 0) { return rtt; }
  }
  return -1;
}

int NetworkManager::PingOnce(std::string_view target_ip, int timeout_ms) {
  const int sock =
      socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sock < 0) { return -1; }

  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(53);
  if (inet_pton(AF_INET, std::string{target_ip}.c_str(), &dest.sin_addr) != 1) {
    close(sock);
    return -1;
  }

  const auto t_start = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&] {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start)
            .count());
  };

  const int rc =
      connect(sock, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

  if (rc < 0 && errno != EINPROGRESS) {
    const int rtt = elapsed_ms();
    close(sock);
    if (errno == ECONNREFUSED) {
      log_.debug("TCP probe: {} refused (reachable) rtt={}ms", target_ip, rtt);
      return rtt;
    }
    return -1;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);
  struct timeval tv {
    timeout_ms / 1000, (timeout_ms % 1000) * 1000
  };

  if (select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
    close(sock);
    return -1;
  }

  int err = 0;
  socklen_t err_len = sizeof(err);
  getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len);
  const int rtt = elapsed_ms();
  close(sock);

  if (err != 0 && err != ECONNREFUSED) { return -1; }

  return rtt;
}

// ---------------------------------------------------------------------------
// GetNetworkInfo
// ---------------------------------------------------------------------------

NetworkManager::NetworkInfo NetworkManager::GetNetworkInfo() {
  NetworkInfo info;

  for (auto [i, iface] : std::views::enumerate(config_.interfaces)) {
    if (iface_states_[i] != InterfaceState::kUp) { continue; }

    const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) { continue; }

    struct ifreq ifr {};
    strncpy(ifr.ifr_name, iface.name.c_str(), IFNAMSIZ - 1);

    char buf[INET_ADDRSTRLEN]{};

    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
      inet_ntop(AF_INET,
                &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr,
                buf,
                sizeof(buf));
      info.ip_address = buf;
    }

    if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
      auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
      inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
      info.netmask = buf;

      uint32_t mask = ntohl(sa->sin_addr.s_addr);
      int prefix = 0;
      while (mask & 0x80000000u) {
        ++prefix;
        mask <<= 1;
      }
      info.prefix_len = prefix;
    }

    if (ioctl(sock, SIOCGIFBRDADDR, &ifr) == 0) {
      inet_ntop(
          AF_INET,
          &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_broadaddr)->sin_addr,
          buf,
          sizeof(buf));
      info.broadcast = buf;
    }

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
      const auto* hw =
          reinterpret_cast<const unsigned char*>(ifr.ifr_hwaddr.sa_data);
      info.mac_address =
          std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                      hw[0],
                      hw[1],
                      hw[2],
                      hw[3],
                      hw[4],
                      hw[5]);
    }

    close(sock);
    info.interface = iface.name;

    std::ifstream route_file{"/proc/net/route"};
    std::string line;
    std::getline(route_file, line);  // skip header
    while (std::getline(route_file, line)) {
      std::istringstream ss{line};
      std::string iface_col, dest_col, gw_col;
      ss >> iface_col >> dest_col >> gw_col;

      if (iface_col != iface.name || dest_col != "00000000") { continue; }

      struct in_addr gw_addr {};
      gw_addr.s_addr = static_cast<uint32_t>(std::stoul(gw_col, nullptr, 16));
      inet_ntop(AF_INET, &gw_addr, buf, sizeof(buf));
      info.gateway = buf;
      break;
    }

    std::ifstream resolv{"/etc/resolv.conf"};
    while (std::getline(resolv, line)) {
      if (line.starts_with("nameserver ")) {
        info.dns_servers.push_back(line.substr(11));
      }
    }

    break;
  }

  {
    std::lock_guard lock{status_mutex_};
    info.online = status_.online;
    info.rtt_ms = status_.rtt_ms;
  }

  return info;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string NetworkManager::FirstUpInterface() const {
  for (auto [i, state] : std::views::enumerate(iface_states_)) {
    if (state == InterfaceState::kUp) { return config_.interfaces[i].name; }
  }
  return "";
}

int NetworkManager::GetInterfaceIndex(std::string_view iface_name) {
  const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (sock < 0) { return -1; }

  struct ifreq ifr {};
  strncpy(ifr.ifr_name, std::string{iface_name}.c_str(), IFNAMSIZ - 1);

  const int ret = ioctl(sock, SIOCGIFINDEX, &ifr);
  close(sock);
  return ret < 0 ? -1 : ifr.ifr_ifindex;
}

}  // namespace aember::network
