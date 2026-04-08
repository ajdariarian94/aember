/**
 * @file network_manager.cpp
 * @author Arian Ajdari
 * @brief Library implementation for NetworkManager.
 *        Brings up network interfaces (netlink + udhcpc fallback),
 *        creates bridge interfaces for LXC containers, and monitors
 *        internet connectivity via TCP probe.
 * @version 0.2
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/network-manager/network-manager.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace aember::network {

// ---------------------------------------------------------------------------
// Internal helpers (file-local)
// ---------------------------------------------------------------------------

namespace {

int RunCommand(const std::vector<std::string>& args) {
  if (args.empty()) { return -1; }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) { return -1; }

  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
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
  auto slash = cidr.find('/');
  if (slash == std::string::npos) { return false; }

  std::string ip_str = cidr.substr(0, slash);
  std::string len_str = cidr.substr(slash + 1);

  if (inet_pton(AF_INET, ip_str.c_str(), &addr) != 1) { return false; }
  prefix_len = std::stoi(len_str);
  return (prefix_len >= 0 && prefix_len <= 32);
}

in_addr PrefixToNetmask(int prefix_len) {
  in_addr mask{};
  mask.s_addr = prefix_len ? htonl(~((1u << (32 - prefix_len)) - 1)) : 0;
  return mask;
}

}  // namespace

// ---------------------------------------------------------------------------
// Netlink helper
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

  bool Valid() const { return fd >= 0; }

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
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
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
// Constructor / Destructor
// ---------------------------------------------------------------------------

NetworkManager::NetworkManager(
    const nlohmann::json& config,
    std::function<void(const ConnectivityStatus&)> on_status)
    : on_status_(std::move(on_status)), log_("network-manager") {
  ParseConfig(config);
  iface_states_.assign(config_.interfaces.size(), InterfaceState::kDown);
}

NetworkManager::~NetworkManager() {
  Stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void NetworkManager::Start() {
  log_.info("Starting NetworkManager");

  BringUpInterfaces();
  CreateBridges();

  {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    running_ = true;
  }

  monitor_thread_ = std::thread(&NetworkManager::MonitorLoop, this);
  log_.info("NetworkManager started - connectivity monitor running");
}

void NetworkManager::Stop() {
  {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    running_ = false;
  }
  cv_.notify_all();

  if (monitor_thread_.joinable()) { monitor_thread_.join(); }

  log_.info("NetworkManager stopped");
}

ConnectivityStatus NetworkManager::GetStatus() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_;
}

bool NetworkManager::IsOnline() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_.online;
}

bool NetworkManager::WaitForConnectivity(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  log_.info("Waiting for connectivity (timeout {}ms)", timeout.count());

  while (std::chrono::steady_clock::now() < deadline) {
    if (IsOnline()) {
      log_.info("Connectivity confirmed");
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
      std::chrono::milliseconds(config.value("ping_interval_ms", 10000));
  config_.dhcp_timeout =
      std::chrono::milliseconds(config.value("dhcp_timeout_ms", 30000));
  config_.dhcp_retries = config.value("dhcp_retries", 3);

  if (!config.contains("interfaces") || !config["interfaces"].is_array()) {
    throw std::runtime_error(
        "NetworkManager: config missing 'interfaces' array");
  }

  for (const auto& entry : config["interfaces"]) {
    InterfaceConfig iface;
    iface.name = entry.at("name").get<std::string>();
    iface.required = entry.value("required", false);

    std::string mode_str = entry.value("mode", "dhcp");
    if (mode_str == "static") {
      iface.mode = IpMode::kStatic;
      iface.address = entry.at("address").get<std::string>();
      iface.gateway = entry.value("gateway", "");
    } else {
      iface.mode = IpMode::kDhcp;
    }

    if (entry.contains("dns")) {
      for (const auto& dns : entry["dns"]) {
        iface.dns_servers.push_back(dns.get<std::string>());
      }
    }

    log_.info("Parsed interface: {} mode={} required={}",
              iface.name,
              mode_str,
              iface.required);
    config_.interfaces.push_back(std::move(iface));
  }

  // Parse bridges (optional)
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

  for (size_t i = 0; i < config_.interfaces.size(); ++i) {
    const auto& iface = config_.interfaces[i];
    iface_states_[i] = InterfaceState::kBringingUp;

    bool ok = BringUpInterface(iface);

    iface_states_[i] = ok ? InterfaceState::kUp : InterfaceState::kFailed;

    if (!ok) {
      if (iface.required) {
        throw std::runtime_error("NetworkManager: required interface '" +
                                 iface.name + "' failed to come up");
      }
      log_.warn("Interface {} failed to come up (not required, continuing)",
                iface.name);
    } else {
      log_.info("Interface {} is up", iface.name);
      WriteResolvConf(iface);
    }
  }
}

bool NetworkManager::BringUpInterface(const InterfaceConfig& iface) {
  log_.info("Bringing up interface: {} (mode={})",
            iface.name,
            iface.mode == IpMode::kDhcp ? "dhcp" : "static");

  if (!NetlinkSetInterfaceUp(iface.name)) {
    log_.warn("{}: netlink UP failed, trying fallback", iface.name);
    if (!FallbackIpCommand({"ip", "link", "set", iface.name, "up"})) {
      log_.error("{}: could not set interface UP", iface.name);
      return false;
    }
  }

  if (iface.mode == IpMode::kStatic) {
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

bool NetworkManager::NetlinkSetInterfaceUp(const std::string& iface_name) {
  NetlinkSocket nl;
  if (!nl.Valid()) {
    log_.error("Failed to open netlink socket: {}", strerror(errno));
    return false;
  }

  int iface_index = GetInterfaceIndex(iface_name);
  if (iface_index < 0) {
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
  req.ifi.ifi_index = iface_index;
  req.ifi.ifi_flags = IFF_UP;
  req.ifi.ifi_change = IFF_UP;

  bool ok = nl.SendAndAck(&req.nlh);
  if (!ok) { log_.debug("NetlinkSetInterfaceUp failed for {}", iface_name); }
  return ok;
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

  int iface_index = GetInterfaceIndex(iface.name);
  if (iface_index < 0) { return false; }

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
  req.ifa.ifa_index = iface_index;

  struct rtattr* rta = reinterpret_cast<struct rtattr*>(
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
  rtreq.rtm.rtm_dst_len = 0;
  rtreq.rtm.rtm_src_len = 0;
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
  memcpy(RTA_DATA(rta), &iface_index, sizeof(int));
  rtreq.nlh.nlmsg_len = NLMSG_ALIGN(rtreq.nlh.nlmsg_len) + rta->rta_len;

  if (!nl.SendAndAck(&rtreq.nlh)) {
    log_.warn("{}: RTM_NEWROUTE failed, continuing anyway", iface.name);
  } else {
    log_.debug("{}: default route via {} set", iface.name, iface.gateway);
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

    int timeout_sec = static_cast<int>(config_.dhcp_timeout.count() / 1000);
    std::string timeout_str = std::to_string(std::max(1, timeout_sec));

    int ret = RunCommand({
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
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }

  log_.error("{}: all DHCP attempts exhausted", iface.name);
  return false;
}

// ---------------------------------------------------------------------------
// Fallback ip command
// ---------------------------------------------------------------------------

bool NetworkManager::FallbackIpCommand(const std::vector<std::string>& args) {
  std::ostringstream oss;
  for (const auto& a : args) { oss << a << ' '; }
  log_.debug("Fallback ip command: {}", oss.str());

  int ret = RunCommand(args);
  if (ret != 0) {
    log_.warn("ip command failed (exit {}): {}", ret, oss.str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// resolv.conf
// ---------------------------------------------------------------------------

void NetworkManager::WriteResolvConf(const InterfaceConfig& iface) {
  std::vector<std::string> servers = iface.dns_servers;
  if (servers.empty()) {
    servers = {"8.8.8.8", "1.1.1.1"};
    log_.debug("No DNS configured for {}, using defaults", iface.name);
  }

  std::ofstream f("/etc/resolv.conf", std::ios::trunc);
  if (!f.is_open()) {
    log_.warn("Could not write /etc/resolv.conf");
    return;
  }

  f << "# Generated by aember NetworkManager\n";
  for (const auto& ns : servers) { f << "nameserver " << ns << "\n"; }

  log_.info("Wrote /etc/resolv.conf ({} nameserver(s))", servers.size());
}

// ---------------------------------------------------------------------------
// Bridge management
// ---------------------------------------------------------------------------

void NetworkManager::CreateBridges() {
  if (config_.bridges.empty()) {
    log_.debug("No bridges configured, skipping");
    return;
  }

  log_.info("Creating {} bridge(s)", config_.bridges.size());

  for (const auto& bridge : config_.bridges) {
    if (!CreateBridge(bridge)) {
      log_.warn("Failed to create bridge '{}', continuing", bridge.name);
    }
  }
}

bool NetworkManager::CreateBridge(const BridgeConfig& bridge) {
  log_.info("Creating bridge: {} address={}", bridge.name, bridge.address);

  // Skip if bridge already exists
  if (InterfaceExists(bridge.name)) {
    log_.info("Bridge '{}' already exists, skipping creation", bridge.name);
    // Still ensure it's up and has correct address
    FallbackIpCommand({"ip", "link", "set", bridge.name, "up"});
    return true;
  }

  // Create the bridge interface
  if (!FallbackIpCommand(
          {"ip", "link", "add", bridge.name, "type", "bridge"})) {
    log_.error("Failed to create bridge '{}'", bridge.name);
    return false;
  }

  // Assign IP address
  if (!bridge.address.empty()) {
    if (!FallbackIpCommand(
            {"ip", "addr", "add", bridge.address, "dev", bridge.name})) {
      log_.error("Failed to assign address '{}' to bridge '{}'",
                 bridge.address,
                 bridge.name);
      // Don't fail entirely — bridge exists, address assignment failed
    }
  }

  // Bring it up
  if (!FallbackIpCommand({"ip", "link", "set", bridge.name, "up"})) {
    log_.error("Failed to bring up bridge '{}'", bridge.name);
    return false;
  }

  log_.info(
      "Bridge '{}' created and up (address={})", bridge.name, bridge.address);
  return true;
}

bool NetworkManager::InterfaceExists(const std::string& name) {
  return GetInterfaceIndex(name) >= 0;
}

// ---------------------------------------------------------------------------
// Connectivity monitor
// ---------------------------------------------------------------------------

void NetworkManager::MonitorLoop() {
  log_.info("Connectivity monitor started (target={}, interval={}ms)",
            config_.ping_target,
            config_.ping_interval.count());

  std::unique_lock<std::mutex> lock(cv_mutex_);

  while (running_) {
    lock.unlock();

    int rtt = Ping(config_.ping_target);
    UpdateStatus(rtt >= 0, rtt);

    lock.lock();
    cv_.wait_for(lock, config_.ping_interval, [this] { return !running_; });
  }

  log_.info("Connectivity monitor stopped");
}

void NetworkManager::UpdateStatus(bool online, int rtt_ms) {
  ConnectivityStatus s;
  s.online = online;
  s.rtt_ms = rtt_ms;
  s.interface = FirstUpInterface();
  s.timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    bool was_online = status_.online;
    status_ = s;

    if (was_online && !online) {
      log_.warn("Internet connectivity LOST (last seen via {})", s.interface);
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
// TCP connectivity probe
// ---------------------------------------------------------------------------

int NetworkManager::Ping(const std::string& target_ip) {
  for (int i = 0; i < config_.ping_retries; ++i) {
    int rtt = PingOnce(target_ip);
    if (rtt >= 0) { return rtt; }
  }
  return -1;
}

int NetworkManager::PingOnce(const std::string& target_ip, int timeout_ms) {
  int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    log_.error("TCP probe: socket() failed: {}", strerror(errno));
    return -1;
  }

  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(53);
  if (inet_pton(AF_INET, target_ip.c_str(), &dest.sin_addr) != 1) {
    close(sock);
    log_.error("TCP probe: invalid target '{}'", target_ip);
    return -1;
  }

  auto t_start = std::chrono::steady_clock::now();

  int rc =
      connect(sock, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

  if (rc < 0 && errno != EINPROGRESS) {
    if (errno == ECONNREFUSED) {
      close(sock);
      int rtt = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t_start)
              .count());
      log_.debug("TCP probe: {} refused (reachable) rtt={}ms", target_ip, rtt);
      return rtt;
    }
    close(sock);
    log_.debug("TCP probe: connect() failed: {}", strerror(errno));
    return -1;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);
  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  rc = select(sock + 1, nullptr, &wfds, nullptr, &tv);

  int rtt =
      static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count());

  if (rc <= 0) {
    close(sock);
    log_.debug("TCP probe: timeout after {}ms to {}", timeout_ms, target_ip);
    return -1;
  }

  int err = 0;
  socklen_t err_len = sizeof(err);
  getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len);
  close(sock);

  if (err != 0 && err != ECONNREFUSED) {
    log_.debug("TCP probe: async connect error: {}", strerror(err));
    return -1;
  }

  log_.debug("TCP probe: {} reachable rtt={}ms", target_ip, rtt);
  return rtt;
}

// ---------------------------------------------------------------------------
// Network info
// ---------------------------------------------------------------------------

NetworkInfo NetworkManager::GetNetworkInfo() {
  NetworkInfo info;

  for (size_t i = 0; i < config_.interfaces.size(); ++i) {
    if (iface_states_[i] != InterfaceState::kUp) { continue; }

    const std::string& name = config_.interfaces[i].name;

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) { continue; }

    struct ifreq ifr {};
    strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
      auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
      char buf[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
      info.ip_address = buf;
    }

    if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
      auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
      char buf[INET_ADDRSTRLEN]{};
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
      auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_broadaddr);
      char buf[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
      info.broadcast = buf;
    }

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
      const unsigned char* hw =
          reinterpret_cast<const unsigned char*>(ifr.ifr_hwaddr.sa_data);
      char mac[18]{};
      snprintf(mac,
               sizeof(mac),
               "%02x:%02x:%02x:%02x:%02x:%02x",
               hw[0],
               hw[1],
               hw[2],
               hw[3],
               hw[4],
               hw[5]);
      info.mac_address = mac;
    }

    close(sock);
    info.interface = name;

    std::ifstream route_file("/proc/net/route");
    std::string line;
    std::getline(route_file, line);
    while (std::getline(route_file, line)) {
      std::istringstream ss(line);
      std::string iface_col, dest_col, gw_col;
      ss >> iface_col >> dest_col >> gw_col;

      if (iface_col != name) { continue; }
      if (dest_col != "00000000") { continue; }

      uint32_t gw_hex = static_cast<uint32_t>(std::stoul(gw_col, nullptr, 16));
      struct in_addr gw_addr {};
      gw_addr.s_addr = gw_hex;
      char buf[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &gw_addr, buf, sizeof(buf));
      info.gateway = buf;
      break;
    }

    std::ifstream resolv("/etc/resolv.conf");
    while (std::getline(resolv, line)) {
      if (line.rfind("nameserver ", 0) == 0) {
        info.dns_servers.push_back(line.substr(11));
      }
    }

    break;
  }

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    info.online = status_.online;
    info.rtt_ms = status_.rtt_ms;
  }

  log_.debug("NetworkInfo: iface={} ip={} gw={} online={}",
             info.interface,
             info.ip_address,
             info.gateway,
             info.online);

  return info;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string NetworkManager::FirstUpInterface() const {
  for (size_t i = 0; i < iface_states_.size(); ++i) {
    if (iface_states_[i] == InterfaceState::kUp) {
      return config_.interfaces[i].name;
    }
  }
  return "";
}

int NetworkManager::GetInterfaceIndex(const std::string& iface_name) {
  int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (sock < 0) { return -1; }

  struct ifreq ifr {};
  strncpy(ifr.ifr_name, iface_name.c_str(), IFNAMSIZ - 1);

  int ret = ioctl(sock, SIOCGIFINDEX, &ifr);
  close(sock);

  if (ret < 0) { return -1; }

  return ifr.ifr_ifindex;
}

}  // namespace aember::network
