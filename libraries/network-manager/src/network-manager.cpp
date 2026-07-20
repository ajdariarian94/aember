/**
 * @file network-manager.cpp
 * @author Arian Ajdari
 * @brief NetworkManager implementation.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

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
#include <sys/wait.h>
#include <unistd.h>

namespace aember::network {

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace {

int RunCommand(const std::vector<std::string>& args) {
  if (args.empty()) { return -1; }

  // Build argv — LXC C API pattern, const_cast unavoidable.
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  std::ranges::transform(args, std::back_inserter(argv), [](const auto& a) {
    return const_cast<char*>(a.c_str());
  });
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) { return -1; }

  if (pid == 0) {
    if (const int devnull = open("/dev/null", O_WRONLY); devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

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
// Netlink RAII socket
// ---------------------------------------------------------------------------

namespace {

struct NetlinkSocket {
  int fd{-1};

  NetlinkSocket() {
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  }

  ~NetlinkSocket() {
    if (fd >= 0) { close(fd); }
  }

  [[nodiscard]] bool Valid() const { return fd >= 0; }

  bool SendAndAck(struct nlmsghdr* nlh) {
    if (!Valid()) { return false; }

    struct sockaddr_nl sa {};
    sa.nl_family = AF_NETLINK;

    struct iovec iov {
      nlh, nlh->nlmsg_len
    };
    struct msghdr msg {};
    msg.msg_name = &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (sendmsg(fd, &msg, 0) < 0) { return false; }

    alignas(NLMSG_ALIGNTO) char buf[4096];
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) { return false; }

    auto* reply = reinterpret_cast<struct nlmsghdr*>(buf);
    if (!NLMSG_OK(reply, static_cast<unsigned>(n))) { return false; }
    if (reply->nlmsg_type == NLMSG_ERROR) {
      auto* err = reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(reply));
      return err->error == 0;
    }
    return true;
  }
};

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
      WriteResolvConf(iface);
    }
  }
}

bool NetworkManager::BringUpInterface(const InterfaceConfig& iface) {
  const auto mode_str =
      iface.mode == aember::utils::network::IpMode::kDhcp ? "dhcp" : "static";
  log_.info("Bringing up interface: {} (mode={})", iface.name, mode_str);

  if (!NetlinkSetInterfaceUp(iface.name)) {
    log_.warn("{}: netlink UP failed, trying fallback", iface.name);
    if (!FallbackIpCommand({"ip", "link", "set", iface.name, "up"})) {
      log_.error("{}: could not set interface UP", iface.name);
      return false;
    }
  }

  if (iface.mode == aember::utils::network::IpMode::kStatic) {
    if (!NetlinkSetStaticAddress(iface)) {
      log_.warn("{}: netlink static address failed, trying fallback",
                iface.name);
      if (!FallbackIpCommand(
              {"ip", "addr", "add", iface.address, "dev", iface.name})) {
        log_.error("{}: could not assign static address {}",
                   iface.name,
                   iface.address);
        return false;
      }
      if (!iface.gateway.empty()) {
        FallbackIpCommand({"ip",
                           "route",
                           "add",
                           "default",
                           "via",
                           iface.gateway,
                           "dev",
                           iface.name});
      }
    }
    return true;
  }

  return RunUdhcpc(iface);
}

// ---------------------------------------------------------------------------
// Netlink: set interface UP
// ---------------------------------------------------------------------------

bool NetworkManager::NetlinkSetInterfaceUp(std::string_view iface_name) {
  NetlinkSocket nl;
  if (!nl.Valid()) {
    log_.error("Failed to open netlink socket: {}", strerror(errno));
    return false;
  }

  const int idx = GetInterfaceIndex(iface_name);
  if (idx < 0) {
    log_.error("Interface {} not found", iface_name);
    return false;
  }

  struct {
    struct nlmsghdr nlh;
    struct ifinfomsg ifi;
  } req{};
  req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
  req.nlh.nlmsg_type = RTM_NEWLINK;
  req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.nlh.nlmsg_seq = 1;
  req.ifi.ifi_family = AF_UNSPEC;
  req.ifi.ifi_index = idx;
  req.ifi.ifi_flags = IFF_UP;
  req.ifi.ifi_change = IFF_UP;

  return nl.SendAndAck(&req.nlh);
}

// ---------------------------------------------------------------------------
// Netlink: assign static address + default route
// ---------------------------------------------------------------------------

bool NetworkManager::NetlinkSetStaticAddress(const InterfaceConfig& iface) {
  NetlinkSocket nl;
  if (!nl.Valid()) { return false; }

  in_addr addr{};
  int prefix_len = 0;
  if (!ParseCidr(iface.address, addr, prefix_len)) {
    log_.error("{}: invalid CIDR '{}'", iface.name, iface.address);
    return false;
  }

  const int idx = GetInterfaceIndex(iface.name);
  if (idx < 0) { return false; }

  struct {
    struct nlmsghdr nlh;
    struct ifaddrmsg ifa;
    char attrbuf[256];
  } req{};

  req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
  req.nlh.nlmsg_type = RTM_NEWADDR;
  req.nlh.nlmsg_flags =
      NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
  req.nlh.nlmsg_seq = 2;
  req.ifa.ifa_family = AF_INET;
  req.ifa.ifa_prefixlen = static_cast<unsigned char>(prefix_len);
  req.ifa.ifa_index = idx;

  auto* rta = reinterpret_cast<struct rtattr*>(
      reinterpret_cast<char*>(&req.nlh) + NLMSG_ALIGN(req.nlh.nlmsg_len));
  rta->rta_type = IFA_LOCAL;
  rta->rta_len = RTA_LENGTH(sizeof(in_addr));
  memcpy(RTA_DATA(rta), &addr, sizeof(in_addr));
  req.nlh.nlmsg_len = NLMSG_ALIGN(req.nlh.nlmsg_len) + rta->rta_len;

  if (!nl.SendAndAck(&req.nlh)) {
    log_.error("{}: RTM_NEWADDR failed", iface.name);
    return false;
  }

  if (iface.gateway.empty()) { return true; }

  in_addr gw{};
  if (inet_pton(AF_INET, iface.gateway.c_str(), &gw) != 1) {
    log_.warn(
        "{}: invalid gateway '{}', skipping route", iface.name, iface.gateway);
    return true;
  }

  struct {
    struct nlmsghdr nlh;
    struct rtmsg rtm;
    char attrbuf[256];
  } rtreq{};

  rtreq.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  rtreq.nlh.nlmsg_type = RTM_NEWROUTE;
  rtreq.nlh.nlmsg_flags =
      NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
  rtreq.nlh.nlmsg_seq = 3;
  rtreq.rtm.rtm_family = AF_INET;
  rtreq.rtm.rtm_table = RT_TABLE_MAIN;
  rtreq.rtm.rtm_protocol = RTPROT_STATIC;
  rtreq.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
  rtreq.rtm.rtm_type = RTN_UNICAST;

  rta = reinterpret_cast<struct rtattr*>(reinterpret_cast<char*>(&rtreq.nlh) +
                                         NLMSG_ALIGN(rtreq.nlh.nlmsg_len));
  rta->rta_type = RTA_GATEWAY;
  rta->rta_len = RTA_LENGTH(sizeof(in_addr));
  memcpy(RTA_DATA(rta), &gw, sizeof(in_addr));
  rtreq.nlh.nlmsg_len = NLMSG_ALIGN(rtreq.nlh.nlmsg_len) + rta->rta_len;

  rta = reinterpret_cast<struct rtattr*>(reinterpret_cast<char*>(&rtreq.nlh) +
                                         NLMSG_ALIGN(rtreq.nlh.nlmsg_len));
  rta->rta_type = RTA_OIF;
  rta->rta_len = RTA_LENGTH(sizeof(int));
  memcpy(RTA_DATA(rta), &idx, sizeof(int));
  rtreq.nlh.nlmsg_len = NLMSG_ALIGN(rtreq.nlh.nlmsg_len) + rta->rta_len;

  if (!nl.SendAndAck(&rtreq.nlh)) {
    log_.warn("{}: RTM_NEWROUTE failed, continuing anyway", iface.name);
  }

  return true;
}

// ---------------------------------------------------------------------------
// DHCP via udhcpc
// ---------------------------------------------------------------------------

bool NetworkManager::RunUdhcpc(const InterfaceConfig& iface) {
  for (int attempt = 1; attempt <= config_.dhcp_retries; ++attempt) {
    log_.info(
        "{}: DHCP attempt {}/{}", iface.name, attempt, config_.dhcp_retries);

    const auto timeout_str = std::to_string(
        std::max(1, static_cast<int>(config_.dhcp_timeout.count() / 1000)));

    const int ret = RunCommand({
        "udhcpc",
        "-i",
        iface.name,
        "-n",
        "-q",
        "-t",
        "5",
        "-T",
        timeout_str,
        "-s",
        "/usr/share/udhcpc/default.script",
    });

    if (ret == 0) {
      log_.info("{}: DHCP lease obtained", iface.name);
      return true;
    }

    log_.warn(
        "{}: udhcpc attempt {} failed (exit {})", iface.name, attempt, ret);
    if (attempt < config_.dhcp_retries) {
      std::this_thread::sleep_for(std::chrono::seconds{2});
    }
  }

  log_.error("{}: all DHCP attempts exhausted", iface.name);
  return false;
}

// ---------------------------------------------------------------------------
// Fallback ip command
// ---------------------------------------------------------------------------

bool NetworkManager::FallbackIpCommand(const std::vector<std::string>& args) {
  std::string cmd;
  for (const auto& a : args) {
    cmd += a;
    cmd += ' ';
  }
  log_.debug("Fallback: {}", cmd);

  if (const int ret = RunCommand(args); ret != 0) {
    log_.warn("ip command failed (exit {}): {}", ret, cmd);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// resolv.conf
// ---------------------------------------------------------------------------

void NetworkManager::WriteResolvConf(const InterfaceConfig& iface) {
  const auto& servers = !iface.dns_servers.empty()
                            ? iface.dns_servers
                            : std::vector<std::string>{"8.8.8.8", "1.1.1.1"};

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

  if (InterfaceExists(bridge.name)) {
    log_.info("Bridge '{}' already exists, skipping creation", bridge.name);
    FallbackIpCommand({"ip", "link", "set", bridge.name, "up"});
    return true;
  }

  if (!FallbackIpCommand(
          {"ip", "link", "add", bridge.name, "type", "bridge"})) {
    log_.error("Failed to create bridge '{}'", bridge.name);
    return false;
  }

  if (!bridge.address.empty()) {
    if (!FallbackIpCommand(
            {"ip", "addr", "add", bridge.address, "dev", bridge.name})) {
      log_.error(
          "Failed to assign '{}' to bridge '{}'", bridge.address, bridge.name);
    }
  }

  if (!FallbackIpCommand({"ip", "link", "set", bridge.name, "up"})) {
    log_.error("Failed to bring up bridge '{}'", bridge.name);
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
    UpdateStatus(Ping(config_.ping_target) >= 0, Ping(config_.ping_target));
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

  log_.debug("TCP probe: {} reachable rtt={}ms", target_ip, rtt);
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
