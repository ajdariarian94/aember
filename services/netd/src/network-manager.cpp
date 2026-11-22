/**
 * @file network-manager.cpp
 * @author Arian Ajdari
 * @brief NetworkManager implementation.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/netlink/netlink.h>
#include <aember-services/netd/dhcp-client.h>
#include <aember-services/netd/network-manager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace aember::netd {

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace {

namespace netlink = aember::utils::netlink;

/// A waiting interface is tried again on the next MaintainLeases with a link;
/// a failed one only this often.
constexpr auto kRetryFailed = std::chrono::seconds{60};
/// How long bring-up waits for a link before it calls the interface waiting
/// (NICs need a moment for autonegotiation after link up).
constexpr auto kCarrierWait = std::chrono::seconds{3};

/// The link is up and usable (cable, Wi-Fi after the WPA handshake).
/// Not IFF_RUNNING: that is also set while the operational state is
/// "unknown", which a Wi-Fi interface briefly is before it associated.
bool HasCarrier(std::string_view name) {
  return netlink::LinkUsable(name);
}

bool WaitForCarrier(std::string_view name, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!HasCarrier(name)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

NetworkManager::NetworkManager(const nlohmann::json& config,
                               StatusCallback on_status)
    : on_status_(std::move(on_status)) {
  ParseConfig(config);
  // Value-initialized: every state starts as kDown (0).
  iface_states_ =
      std::vector<std::atomic<InterfaceState>>(config_.interfaces.size());
  next_retry_.assign(config_.interfaces.size(), {});
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
      iface.mode = aember::netd::IpMode::kStatic;
      iface.address = entry.at("address").get<std::string>();
      iface.gateway = entry.value("gateway", "");
    } else {
      iface.mode = aember::netd::IpMode::kDhcp;
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

  if (config.contains("bridges")) {
    // Container bridges belong to the containers: aember-containerd creates
    // them (containers.json), so they exist with any network manager.
    log_.warn(
        "network.json 'bridges' is ignored: list them in containers.json");
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

    const auto state = BringUpInterface(iface);
    iface_states_[i] = state;
    next_retry_[i] = std::chrono::steady_clock::now() + kRetryFailed;

    if (state == InterfaceState::kFailed) {
      if (iface.required) {
        throw std::runtime_error(std::format(
            "NetworkManager: required interface '{}' failed to come up",
            iface.name));
      }
      log_.warn("Interface {} failed (not required, retried later)",
                iface.name);
    } else if (state == InterfaceState::kWaiting) {
      log_.info("Interface {}: no link yet, brought up when it appears",
                iface.name);
    } else {
      log_.info("Interface {} is up", iface.name);
    }
  }
}

void NetworkManager::RetryPendingInterfaces() {
  const auto now = std::chrono::steady_clock::now();
  for (auto [i, iface] : std::views::enumerate(config_.interfaces)) {
    const auto state = iface_states_[i].load();
    if (state != InterfaceState::kWaiting && state != InterfaceState::kFailed) {
      continue;
    }
    // A waiting interface is tried as soon as it has a link; a failed one
    // (no such interface, DHCP gave up) only every kRetryFailed.
    if (state == InterfaceState::kWaiting ? !HasCarrier(iface.name)
                                          : now < next_retry_[i]) {
      continue;
    }
    next_retry_[i] = now + kRetryFailed;
    log_.info("Interface {}: trying again", iface.name);
    const auto next = BringUpInterface(iface, 1);
    iface_states_[i] = next;
    if (next == InterfaceState::kUp) {
      log_.info("Interface {} is up", iface.name);
    }
  }
}

NetworkManager::InterfaceState NetworkManager::StateOf(
    std::string_view name) const {
  for (auto [i, iface] : std::views::enumerate(config_.interfaces)) {
    if (iface.name == name) return iface_states_[i].load();
  }
  return InterfaceState::kDown;
}

NetworkManager::InterfaceState NetworkManager::BringUpInterface(
    const InterfaceConfig& iface, int dhcp_attempts) {
  const auto mode_str =
      iface.mode == aember::netd::IpMode::kDhcp ? "dhcp" : "static";
  log_.info("Bringing up interface: {} (mode={})", iface.name, mode_str);

  const int idx = netlink::InterfaceIndex(iface.name);
  if (idx < 0) {
    log_.error("{}: no such interface", iface.name);
    return InterfaceState::kFailed;
  }
  if (const int rc = netlink::SetLinkUp(idx); rc != 0) {
    log_.error("{}: could not set link up: {}",
               iface.name,
               std::system_category().message(-rc));
    return InterfaceState::kFailed;
  }

  // Already configured (a container runtime like Docker, the kernel's ip=
  // parameter, a previous aember): keep it rather than running DHCP over it.
  if (const auto existing = netlink::FirstIPv4Address(iface.name)) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &*existing, ip, sizeof(ip));
    log_.info("{}: keeping existing address {}", iface.name, ip);
    if (!iface.dns_servers.empty()) { WriteResolvConf(iface.dns_servers); }
    if (iface.mode == aember::netd::IpMode::kDhcp) {
      AdoptAddress(iface, idx, *existing);
    }
    return InterfaceState::kUp;
  }

  const auto result = [](bool ok) {
    return ok ? InterfaceState::kUp : InterfaceState::kFailed;
  };
  if (iface.mode == aember::netd::IpMode::kStatic) {
    return result(ConfigureStatic(iface, idx));
  }
  // DHCP needs a link: without one (Wi-Fi before it associated, no cable)
  // it would only time out. A required interface keeps its DHCP attempts
  // (boot waits for it); any other waits for its link.
  if (!iface.required && !WaitForCarrier(iface.name, kCarrierWait)) {
    return InterfaceState::kWaiting;
  }
  return result(ConfigureDhcp(iface, idx, dhcp_attempts));
}

bool NetworkManager::ConfigureStatic(const InterfaceConfig& iface, int idx) {
  in_addr addr{};
  int prefix_len = 0;
  if (!netlink::ParseCidr(iface.address, addr, prefix_len)) {
    log_.error("{}: invalid CIDR '{}'", iface.name, iface.address);
    return false;
  }
  if (const int rc = netlink::AddAddress(idx, addr, prefix_len); rc != 0) {
    log_.error("{}: could not assign {}: {}",
               iface.name,
               iface.address,
               std::system_category().message(-rc));
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
                std::system_category().message(-rc));
    }
  }
  WriteResolvConf(iface.dns_servers);
  return true;
}

bool NetworkManager::ConfigureDhcp(const InterfaceConfig& iface, int idx,
                                   int attempts) {
  if (attempts < 0) attempts = config_.dhcp_retries;
  dhcp::DhcpClient client{iface.name};
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    log_.info("{}: DHCP attempt {}/{}", iface.name, attempt, attempts);
    auto lease = client.Acquire(config_.dhcp_timeout);
    if (!lease) {
      log_.warn("{}: {}", iface.name, lease.error());
      continue;
    }
    if (ApplyLease(iface, idx, *lease)) return true;
  }
  log_.error("{}: all DHCP attempts exhausted", iface.name);
  return false;
}

namespace {

std::string IpString(in_addr a) {
  char buf[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

}  // namespace

bool NetworkManager::ApplyLease(const InterfaceConfig& iface, int idx,
                                const dhcp::Lease& lease,
                                std::optional<dhcp::Lease> previous) {
  const auto ip = IpString(lease.address);
  const int prefix = lease.PrefixLength();
  if (previous && previous->address.s_addr != lease.address.s_addr) {
    log_.warn("{}: address changes {} -> {}",
              iface.name,
              IpString(previous->address),
              ip);
    netlink::DeleteAddress(idx, previous->address, previous->PrefixLength());
  }
  if (const int rc = netlink::AddAddress(idx, lease.address, prefix); rc != 0) {
    log_.error("{}: could not assign {}/{}: {}",
               iface.name,
               ip,
               prefix,
               std::system_category().message(-rc));
    return false;
  }

  std::string router = "none";
  if (lease.router) {
    router = IpString(*lease.router);
    if (const int rc = netlink::AddDefaultRoute(idx, *lease.router); rc != 0) {
      log_.warn("{}: default route via {} failed: {}",
                iface.name,
                router,
                std::system_category().message(-rc));
    }
  }

  // Configured DNS servers win over the ones the lease offers.
  std::vector<std::string> dns = iface.dns_servers;
  if (dns.empty()) {
    for (const auto& d : lease.dns) dns.push_back(IpString(d));
  }
  if (!dns.empty() || !previous) WriteResolvConf(dns);

  auto& state = leases_[iface.name];
  state.iface = iface;
  state.ifindex = idx;
  state.lease = lease;
  state.timer = dhcp::LeaseTimer{lease, std::chrono::steady_clock::now()};
  state.next_try = {};

  log_.info("{}: DHCP lease {}/{} router={} lease={}s{}",
            iface.name,
            ip,
            prefix,
            router,
            lease.lease_seconds,
            previous ? " (renewed)" : "");
  return true;
}

void NetworkManager::AdoptAddress(const InterfaceConfig& iface, int idx,
                                  in_addr address) {
  dhcp::DhcpClient client{iface.name};
  auto lease = client.Confirm(address, std::chrono::seconds{4});
  if (lease) {
    ApplyLease(iface, idx, *lease, dhcp::Lease{.address = address});
    return;
  }
  if (lease.error().nak) {
    log_.warn("{}: DHCP server refused {}: getting a new lease",
              iface.name,
              IpString(address));
    const int prefix = netlink::FirstIPv4Prefix(iface.name);
    netlink::DeleteAddress(idx, address, prefix < 0 ? 32 : prefix);
    ConfigureDhcp(iface, idx);
    return;
  }
  // No DHCP server (a container runtime set the address, ip=, ...): keep
  // it as it is, without a lease to renew.
  log_.info("{}: no DHCP answer for {} ({}): keeping it without a lease",
            iface.name,
            IpString(address),
            lease.error().message);
}

void NetworkManager::MaintainLeases() {
  RetryPendingInterfaces();
  using Phase = dhcp::LeaseTimer::Phase;
  const auto now = std::chrono::steady_clock::now();
  for (auto& [name, state] : leases_) {
    const auto phase = state.timer.At(now);
    if (phase == Phase::kBound || phase == Phase::kInfinite) continue;
    if (now < state.next_try) continue;

    if (phase == Phase::kExpired) {
      log_.warn("{}: lease expired: dropping {} and starting over",
                name,
                IpString(state.lease.address));
      netlink::DeleteAddress(
          state.ifindex, state.lease.address, state.lease.PrefixLength());
      const auto iface = state.iface;
      const int idx = state.ifindex;
      state.next_try = now + std::chrono::seconds{30};
      ConfigureDhcp(iface, idx, 1);  // ApplyLease resets the state
      return;  // leases_ may have changed; the rest waits for the next call
    }

    const bool rebinding = phase == Phase::kRebinding;
    dhcp::DhcpClient client{name};
    auto renewed =
        client.Renew(state.lease, rebinding, std::chrono::seconds{4});
    if (renewed) {
      const auto previous = state.lease;
      const auto iface = state.iface;
      ApplyLease(iface, state.ifindex, *renewed, previous);
      continue;
    }
    if (renewed.error().nak) {
      log_.warn("{}: lease refused (NAK): starting over", name);
      netlink::DeleteAddress(
          state.ifindex, state.lease.address, state.lease.PrefixLength());
      const auto iface = state.iface;
      const int idx = state.ifindex;
      state.next_try = now + std::chrono::seconds{30};
      ConfigureDhcp(iface, idx, 1);
      return;
    }
    // RFC 2131: retry at half the remaining time (at least a minute).
    const auto until =
        rebinding ? state.timer.ExpiresAt() : state.timer.RebindAt();
    state.next_try = now + std::max<std::chrono::steady_clock::duration>(
                               std::chrono::seconds{60}, (until - now) / 2);
    log_.warn(
        "{}: {} failed ({}), next try in {} s",
        name,
        rebinding ? "rebinding" : "renewal",
        renewed.error().message,
        std::chrono::duration_cast<std::chrono::seconds>(state.next_try - now)
            .count());
  }
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
    // Wakes at once on Stop() (it used to sleep out the whole interval).
    std::mutex m;
    std::unique_lock lock{m};
    std::condition_variable_any{}.wait_for(
        lock, stop_token, config_.ping_interval, [] { return false; });
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
    timeout_ms / 1000, static_cast<suseconds_t>(timeout_ms % 1000) * 1000
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
    // ifreq's sockaddr fields hold a sockaddr_in: copy it out instead of
    // casting (sockaddr_in needs more alignment than sockaddr on 32-bit ARM).
    const auto ipv4 = [](const sockaddr& sa) {
      sockaddr_in sin{};
      std::memcpy(&sin, &sa, sizeof(sin));
      return sin.sin_addr;
    };

    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
      const in_addr addr = ipv4(ifr.ifr_addr);
      inet_ntop(AF_INET, &addr, buf, sizeof(buf));
      info.ip_address = buf;
    }

    if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
      const in_addr netmask = ipv4(ifr.ifr_netmask);
      inet_ntop(AF_INET, &netmask, buf, sizeof(buf));
      info.netmask = buf;

      uint32_t mask = ntohl(netmask.s_addr);
      int prefix = 0;
      while (mask & 0x80000000u) {
        ++prefix;
        mask <<= 1;
      }
      info.prefix_len = prefix;
    }

    if (ioctl(sock, SIOCGIFBRDADDR, &ifr) == 0) {
      const in_addr broadcast = ipv4(ifr.ifr_broadaddr);
      inet_ntop(AF_INET, &broadcast, buf, sizeof(buf));
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

}  // namespace aember::netd
