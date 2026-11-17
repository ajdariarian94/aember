/**
 * @file dhcp-client.cpp
 * @author Arian Ajdari
 * @brief Minimal DHCPv4 client (RFC 2131), replacing BusyBox udhcpc.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/network-manager/dhcp-client.h>
#include <aember-libs/network-manager/netlink.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <random>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace aember::network::dhcp {

namespace {

constexpr std::uint16_t kServerPort = 67;
constexpr std::uint16_t kClientPort = 68;
constexpr std::size_t kHeaderSize = 236;  // up to and including `file`
constexpr std::uint8_t kCookie[] = {99, 130, 83, 99};
constexpr std::uint16_t kBroadcastFlag = 0x8000;
constexpr auto kResend = std::chrono::seconds{2};

// Options (RFC 2132)
constexpr std::uint8_t kOptPad = 0;
constexpr std::uint8_t kOptSubnetMask = 1;
constexpr std::uint8_t kOptRouter = 3;
constexpr std::uint8_t kOptDns = 6;
constexpr std::uint8_t kOptRequestedIp = 50;
constexpr std::uint8_t kOptLeaseTime = 51;
constexpr std::uint8_t kOptMessageType = 53;
constexpr std::uint8_t kOptServerId = 54;
constexpr std::uint8_t kOptParamList = 55;
constexpr std::uint8_t kOptEnd = 255;

std::vector<std::uint8_t> Header(std::uint32_t xid, const Mac& mac) {
  std::vector<std::uint8_t> p(kHeaderSize, 0);
  p[0] = 1;  // BOOTREQUEST
  p[1] = 1;  // Ethernet
  p[2] = 6;  // MAC length
  const std::uint32_t nxid = htonl(xid);
  std::memcpy(&p[4], &nxid, 4);
  const std::uint16_t flags = htons(kBroadcastFlag);
  std::memcpy(&p[10], &flags, 2);
  std::memcpy(&p[28], mac.data(), mac.size());
  p.insert(p.end(), std::begin(kCookie), std::end(kCookie));
  return p;
}

void AddOption(std::vector<std::uint8_t>& p, std::uint8_t code,
               const void* data, std::uint8_t len) {
  p.push_back(code);
  p.push_back(len);
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  p.insert(p.end(), bytes, bytes + len);
}

void AddCommonOptions(std::vector<std::uint8_t>& p, MessageType type) {
  const auto t = static_cast<std::uint8_t>(type);
  AddOption(p, kOptMessageType, &t, 1);
  static constexpr std::uint8_t kParams[] = {
      kOptSubnetMask, kOptRouter, kOptDns, kOptLeaseTime, kOptServerId};
  AddOption(p, kOptParamList, kParams, sizeof(kParams));
}

std::optional<Mac> InterfaceMac(int fd, const std::string& iface) {
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
  if (::ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) return std::nullopt;
  Mac mac{};
  std::memcpy(mac.data(), ifr.ifr_hwaddr.sa_data, mac.size());
  return mac;
}

}  // namespace

int Lease::PrefixLength() const {
  if (netmask.s_addr == 0) return 24;
  const int p = netlink::NetmaskToPrefix(netmask);
  return p < 0 ? 24 : p;
}

std::vector<std::uint8_t> BuildDiscover(std::uint32_t xid, const Mac& mac) {
  auto p = Header(xid, mac);
  AddCommonOptions(p, MessageType::kDiscover);
  p.push_back(kOptEnd);
  return p;
}

std::vector<std::uint8_t> BuildRequest(std::uint32_t xid, const Mac& mac,
                                       in_addr requested, in_addr server) {
  auto p = Header(xid, mac);
  AddCommonOptions(p, MessageType::kRequest);
  AddOption(p, kOptRequestedIp, &requested, 4);
  AddOption(p, kOptServerId, &server, 4);
  p.push_back(kOptEnd);
  return p;
}

std::optional<Message> Parse(std::span<const std::uint8_t> p) {
  if (p.size() < kHeaderSize + sizeof(kCookie)) return std::nullopt;
  if (p[0] != 2 || p[1] != 1 || p[2] != 6) return std::nullopt;  // BOOTREPLY
  if (std::memcmp(&p[kHeaderSize], kCookie, sizeof(kCookie)) != 0) {
    return std::nullopt;
  }

  Message m;
  std::uint32_t nxid = 0;
  std::memcpy(&nxid, &p[4], 4);
  m.xid = ntohl(nxid);
  std::memcpy(&m.lease.address, &p[16], 4);  // yiaddr
  std::memcpy(m.chaddr.data(), &p[28], m.chaddr.size());

  bool have_type = false;
  std::size_t i = kHeaderSize + sizeof(kCookie);
  while (i < p.size()) {
    const std::uint8_t code = p[i++];
    if (code == kOptPad) continue;
    if (code == kOptEnd) break;
    if (i >= p.size()) return std::nullopt;
    const std::uint8_t len = p[i++];
    if (i + len > p.size()) return std::nullopt;  // truncated option
    const std::uint8_t* v = &p[i];
    switch (code) {
      case kOptMessageType:
        if (len == 1) {
          m.type = static_cast<MessageType>(v[0]);
          have_type = true;
        }
        break;
      case kOptSubnetMask:
        if (len == 4) std::memcpy(&m.lease.netmask, v, 4);
        break;
      case kOptRouter:
        if (len >= 4) {
          in_addr r{};
          std::memcpy(&r, v, 4);
          m.lease.router = r;
        }
        break;
      case kOptDns:
        for (std::size_t k = 0; k + 4 <= len; k += 4) {
          in_addr d{};
          std::memcpy(&d, v + k, 4);
          m.lease.dns.push_back(d);
        }
        break;
      case kOptLeaseTime:
        if (len == 4) {
          std::uint32_t t = 0;
          std::memcpy(&t, v, 4);
          m.lease.lease_seconds = ntohl(t);
        }
        break;
      case kOptServerId:
        if (len == 4) std::memcpy(&m.lease.server, v, 4);
        break;
      default:
        break;
    }
    i += len;
  }
  if (!have_type) return std::nullopt;
  return m;
}

// ---------------------------------------------------------------------------
// IPv4/UDP framing for the packet socket
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kIpHeader = 20;
constexpr std::size_t kUdpHeader = 8;

/// Internet checksum (RFC 1071) over @p data, continuing from @p sum.
std::uint32_t SumWords(std::span<const std::uint8_t> data,
                       std::uint32_t sum = 0) {
  for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
    sum += static_cast<std::uint32_t>(data[i] << 8 | data[i + 1]);
  }
  if (data.size() % 2 != 0) sum += static_cast<std::uint32_t>(data.back() << 8);
  return sum;
}

std::uint16_t Fold(std::uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<std::uint16_t>(~sum & 0xFFFF);
}

/// UDP checksum over the pseudo header + UDP header + payload.
std::uint16_t UdpChecksum(in_addr src, in_addr dst,
                          std::span<const std::uint8_t> udp) {
  std::uint8_t pseudo[12] = {};
  std::memcpy(pseudo, &src, 4);
  std::memcpy(pseudo + 4, &dst, 4);
  pseudo[9] = IPPROTO_UDP;
  pseudo[10] = static_cast<std::uint8_t>(udp.size() >> 8);
  pseudo[11] = static_cast<std::uint8_t>(udp.size() & 0xFF);
  const std::uint16_t c = Fold(SumWords(udp, SumWords(pseudo)));
  return c == 0 ? 0xFFFF : c;
}

void Put16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

std::uint16_t Get16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

}  // namespace

std::vector<std::uint8_t> BuildUdpFrame(std::span<const std::uint8_t> payload,
                                        in_addr src, in_addr dst,
                                        std::uint16_t src_port,
                                        std::uint16_t dst_port) {
  const std::size_t udp_len = kUdpHeader + payload.size();
  std::vector<std::uint8_t> f(kIpHeader + udp_len, 0);

  std::uint8_t* ip = f.data();
  ip[0] = 0x45;  // IPv4, 20-byte header
  Put16(ip + 2, static_cast<std::uint16_t>(f.size()));
  ip[8] = 64;  // TTL
  ip[9] = IPPROTO_UDP;
  std::memcpy(ip + 12, &src, 4);
  std::memcpy(ip + 16, &dst, 4);
  Put16(ip + 10, Fold(SumWords({ip, kIpHeader})));

  std::uint8_t* udp = ip + kIpHeader;
  Put16(udp, src_port);
  Put16(udp + 2, dst_port);
  Put16(udp + 4, static_cast<std::uint16_t>(udp_len));
  std::memcpy(udp + kUdpHeader, payload.data(), payload.size());
  Put16(udp + 6, UdpChecksum(src, dst, {udp, udp_len}));
  return f;
}

std::span<const std::uint8_t> ParseUdpFrame(std::span<const std::uint8_t> f,
                                            std::uint16_t dst_port) {
  if (f.size() < kIpHeader + kUdpHeader) return {};
  if ((f[0] >> 4) != 4 || f[9] != IPPROTO_UDP) return {};
  const std::size_t ihl = static_cast<std::size_t>(f[0] & 0x0F) * 4;
  const std::size_t total = Get16(&f[2]);
  if (ihl < kIpHeader || total > f.size() || total < ihl + kUdpHeader)
    return {};
  if (Fold(SumWords({f.data(), ihl})) != 0) return {};  // bad IP checksum
  if ((Get16(&f[6]) & 0x1FFF) != 0 || (f[6] & 0x20) != 0)
    return {};  // fragment

  const std::uint8_t* udp = f.data() + ihl;
  if (Get16(udp + 2) != dst_port) return {};
  const std::size_t udp_len = Get16(udp + 4);
  if (udp_len < kUdpHeader || ihl + udp_len > total) return {};
  if (Get16(udp + 6) != 0) {  // 0 = no checksum
    in_addr src{}, dst{};
    std::memcpy(&src, &f[12], 4);
    std::memcpy(&dst, &f[16], 4);
    std::vector<std::uint8_t> copy(udp, udp + udp_len);
    copy[6] = copy[7] = 0;
    if (UdpChecksum(src, dst, copy) != Get16(udp + 6)) return {};
  }
  return {udp + kUdpHeader, udp_len - kUdpHeader};
}

// ---------------------------------------------------------------------------
// DhcpClient
// ---------------------------------------------------------------------------

DhcpClient::DhcpClient(std::string interface)
    : interface_(std::move(interface)) {}

namespace {

/// Wait for a reply of one of @p types for @p xid until @p deadline.
std::optional<Message> Receive(int fd, std::uint32_t xid, const Mac& mac,
                               std::initializer_list<MessageType> types,
                               std::chrono::steady_clock::time_point deadline) {
  std::array<std::uint8_t, 2048> buf{};
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return std::nullopt;
    pollfd pfd{fd, POLLIN, 0};
    const int r = ::poll(&pfd, 1, static_cast<int>(left.count()));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return std::nullopt;
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) continue;
    const auto payload =
        ParseUdpFrame({buf.data(), static_cast<std::size_t>(n)}, kClientPort);
    if (payload.empty()) continue;
    auto msg = Parse(payload);
    if (!msg || msg->xid != xid || msg->chaddr != mac) continue;
    for (const auto t : types) {
      if (msg->type == t) return msg;
    }
  }
}

}  // namespace

std::expected<Lease, std::string> DhcpClient::Acquire(
    std::chrono::milliseconds timeout) {
  const int ifindex = static_cast<int>(::if_nametoindex(interface_.c_str()));
  if (ifindex == 0) {
    return std::unexpected(std::format("no interface {}", interface_));
  }

  const int fd =
      ::socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP));
  if (fd < 0) {
    return std::unexpected(
        std::format("packet socket: {}", std::strerror(errno)));
  }
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};

  sockaddr_ll local{};
  local.sll_family = AF_PACKET;
  local.sll_protocol = htons(ETH_P_IP);
  local.sll_ifindex = ifindex;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    return std::unexpected(
        std::format("bind {}: {}", interface_, std::strerror(errno)));
  }

  const auto mac = InterfaceMac(fd, interface_);
  if (!mac) {
    return std::unexpected(std::format(
        "no MAC address for {}: {}", interface_, std::strerror(errno)));
  }

  sockaddr_ll broadcast = local;
  broadcast.sll_halen = 6;
  std::memset(broadcast.sll_addr, 0xFF, 6);
  in_addr any{};
  in_addr all{};
  all.s_addr = htonl(INADDR_BROADCAST);
  const auto send = [&](const std::vector<std::uint8_t>& dhcp_packet) {
    const auto frame =
        BuildUdpFrame(dhcp_packet, any, all, kClientPort, kServerPort);
    return ::sendto(fd,
                    frame.data(),
                    frame.size(),
                    0,
                    reinterpret_cast<sockaddr*>(&broadcast),
                    sizeof(broadcast)) >= 0;
  };

  std::mt19937 rng{std::random_device{}()};
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    const std::uint32_t xid = rng();
    if (!send(BuildDiscover(xid, *mac))) {
      return std::unexpected(
          std::format("send DISCOVER: {}", std::strerror(errno)));
    }
    const auto round_end =
        std::min(deadline, std::chrono::steady_clock::now() + kResend);
    const auto offer = Receive(fd, xid, *mac, {MessageType::kOffer}, round_end);
    if (!offer) continue;

    if (!send(BuildRequest(
            xid, *mac, offer->lease.address, offer->lease.server))) {
      return std::unexpected(
          std::format("send REQUEST: {}", std::strerror(errno)));
    }
    const auto ack =
        Receive(fd,
                xid,
                *mac,
                {MessageType::kAck, MessageType::kNak},
                std::min(deadline, std::chrono::steady_clock::now() + kResend));
    if (!ack || ack->type == MessageType::kNak) continue;  // start over

    Lease lease = ack->lease;
    // Some servers only put the address in yiaddr of the ACK.
    if (lease.address.s_addr == 0) lease.address = offer->lease.address;
    if (lease.server.s_addr == 0) lease.server = offer->lease.server;
    return lease;
  }
  return std::unexpected(std::format(
      "no DHCP lease on {} within {} ms", interface_, timeout.count()));
}

}  // namespace aember::network::dhcp
