/**
 * @file netlink.cpp
 * @author Arian Ajdari
 * @brief Minimal rtnetlink client (link up, address, default route,
 *        bridge).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/network-manager/netlink.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace aember::network::netlink {

namespace {

/// One netlink request with room for attributes.
struct Request {
  alignas(NLMSG_ALIGNTO) std::array<char, 512> buf{};

  nlmsghdr* Header() { return reinterpret_cast<nlmsghdr*>(buf.data()); }

  template <typename T>
  T* Init(int type, int flags) {
    auto* h = Header();
    h->nlmsg_len = NLMSG_LENGTH(sizeof(T));
    h->nlmsg_type = static_cast<unsigned short>(type);
    h->nlmsg_flags =
        static_cast<unsigned short>(NLM_F_REQUEST | NLM_F_ACK | flags);
    h->nlmsg_seq = 1;
    return reinterpret_cast<T*>(NLMSG_DATA(h));
  }

  /// Append an attribute; returns it (for nesting) or nullptr if full.
  rtattr* Add(int type, const void* data, std::size_t len) {
    auto* h = Header();
    const std::size_t at = NLMSG_ALIGN(h->nlmsg_len);
    const std::size_t total = RTA_LENGTH(len);
    if (at + RTA_ALIGN(total) > buf.size()) return nullptr;
    auto* rta = reinterpret_cast<rtattr*>(buf.data() + at);
    rta->rta_type = static_cast<unsigned short>(type);
    rta->rta_len = static_cast<unsigned short>(total);
    if (len > 0) std::memcpy(RTA_DATA(rta), data, len);
    h->nlmsg_len = static_cast<unsigned>(at + RTA_ALIGN(total));
    return rta;
  }

  /// Close a nested attribute opened with Add(type, nullptr, 0).
  void End(rtattr* nest) {
    nest->rta_len = static_cast<unsigned short>(
        buf.data() + Header()->nlmsg_len - reinterpret_cast<char*>(nest));
  }
};

/// Send and wait for the ACK: 0 or -errno.
int Transact(Request& req) {
  const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) return -errno;

  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  int result = -EIO;
  if (::sendto(fd,
               req.buf.data(),
               req.Header()->nlmsg_len,
               0,
               reinterpret_cast<sockaddr*>(&kernel),
               sizeof(kernel)) < 0) {
    result = -errno;
  } else {
    alignas(NLMSG_ALIGNTO) std::array<char, 4096> reply{};
    const ssize_t n = ::recv(fd, reply.data(), reply.size(), 0);
    if (n < 0) {
      result = -errno;
    } else {
      auto* h = reinterpret_cast<nlmsghdr*>(reply.data());
      if (NLMSG_OK(h, static_cast<unsigned>(n)) &&
          h->nlmsg_type == NLMSG_ERROR) {
        result = reinterpret_cast<nlmsgerr*>(NLMSG_DATA(h))->error;
      }
    }
  }
  ::close(fd);
  return result;
}

}  // namespace

int InterfaceIndex(std::string_view name) {
  const unsigned idx = ::if_nametoindex(std::string{name}.c_str());
  return idx == 0 ? -1 : static_cast<int>(idx);
}

std::optional<in_addr> FirstIPv4Address(std::string_view name) {
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return std::nullopt;
  std::optional<in_addr> found;
  for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
    if (a->ifa_addr != nullptr && a->ifa_addr->sa_family == AF_INET &&
        a->ifa_name != nullptr && name == a->ifa_name) {
      found = reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr;
      break;
    }
  }
  ::freeifaddrs(list);
  return found;
}

int SetLinkUp(int ifindex) {
  Request req;
  auto* ifi = req.Init<ifinfomsg>(RTM_NEWLINK, 0);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  ifi->ifi_flags = IFF_UP;
  ifi->ifi_change = IFF_UP;
  return Transact(req);
}

int AddAddress(int ifindex, in_addr addr, int prefix_len) {
  if (prefix_len < 0 || prefix_len > 32) return -EINVAL;
  Request req;
  auto* ifa = req.Init<ifaddrmsg>(RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE);
  ifa->ifa_family = AF_INET;
  ifa->ifa_prefixlen = static_cast<unsigned char>(prefix_len);
  ifa->ifa_index = static_cast<unsigned>(ifindex);
  req.Add(IFA_LOCAL, &addr, sizeof(addr));
  req.Add(IFA_ADDRESS, &addr, sizeof(addr));
  if (prefix_len < 31) {
    in_addr brd{};
    brd.s_addr = addr.s_addr | ~PrefixToNetmask(prefix_len).s_addr;
    req.Add(IFA_BROADCAST, &brd, sizeof(brd));
  }
  return Transact(req);
}

int AddDefaultRoute(int ifindex, in_addr gateway) {
  Request req;
  auto* rtm = req.Init<rtmsg>(RTM_NEWROUTE, NLM_F_CREATE | NLM_F_REPLACE);
  rtm->rtm_family = AF_INET;
  rtm->rtm_table = RT_TABLE_MAIN;
  rtm->rtm_protocol = RTPROT_BOOT;
  rtm->rtm_scope = RT_SCOPE_UNIVERSE;
  rtm->rtm_type = RTN_UNICAST;
  req.Add(RTA_GATEWAY, &gateway, sizeof(gateway));
  req.Add(RTA_OIF, &ifindex, sizeof(ifindex));
  return Transact(req);
}

int CreateBridge(std::string_view name) {
  if (name.empty() || name.size() >= IFNAMSIZ) return -EINVAL;
  Request req;
  auto* ifi = req.Init<ifinfomsg>(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
  ifi->ifi_family = AF_UNSPEC;
  const std::string ifname{name};
  req.Add(IFLA_IFNAME, ifname.c_str(), ifname.size() + 1);
  auto* linkinfo = req.Add(IFLA_LINKINFO, nullptr, 0);
  if (linkinfo == nullptr) return -EINVAL;
  static constexpr char kKind[] = "bridge";
  req.Add(IFLA_INFO_KIND, kKind, sizeof(kKind));
  req.End(linkinfo);
  return Transact(req);
}

int CreateVethPair(std::string_view name, std::string_view peer) {
  if (name.empty() || name.size() >= IFNAMSIZ || peer.empty() ||
      peer.size() >= IFNAMSIZ) {
    return -EINVAL;
  }
  Request req;
  auto* ifi = req.Init<ifinfomsg>(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
  ifi->ifi_family = AF_UNSPEC;
  const std::string ifname{name};
  req.Add(IFLA_IFNAME, ifname.c_str(), ifname.size() + 1);
  auto* linkinfo = req.Add(IFLA_LINKINFO, nullptr, 0);
  static constexpr char kKind[] = "veth";
  req.Add(IFLA_INFO_KIND, kKind, sizeof(kKind));
  auto* data = req.Add(IFLA_INFO_DATA, nullptr, 0);
  // VETH_INFO_PEER carries a struct ifinfomsg followed by the peer's attrs.
  ifinfomsg peer_ifi{};
  peer_ifi.ifi_family = AF_UNSPEC;
  auto* peer_info =
      req.Add(1 /* VETH_INFO_PEER */, &peer_ifi, sizeof(peer_ifi));
  const std::string peer_name{peer};
  req.Add(IFLA_IFNAME, peer_name.c_str(), peer_name.size() + 1);
  if (linkinfo == nullptr || data == nullptr || peer_info == nullptr) {
    return -EINVAL;
  }
  req.End(peer_info);
  req.End(data);
  req.End(linkinfo);
  return Transact(req);
}

int DeleteLink(std::string_view name) {
  const int idx = InterfaceIndex(name);
  if (idx < 0) return -ENODEV;
  Request req;
  auto* ifi = req.Init<ifinfomsg>(RTM_DELLINK, 0);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = idx;
  return Transact(req);
}

in_addr PrefixToNetmask(int prefix_len) {
  in_addr mask{};
  mask.s_addr = prefix_len <= 0    ? 0
                : prefix_len >= 32 ? 0xFFFFFFFFu
                                   : htonl(~((1u << (32 - prefix_len)) - 1u));
  return mask;
}

int NetmaskToPrefix(in_addr netmask) {
  const uint32_t m = ntohl(netmask.s_addr);
  const int prefix = __builtin_popcount(m);
  // Contiguous: ones then zeros, i.e. the mask equals the one rebuilt from
  // its own popcount.
  return PrefixToNetmask(prefix).s_addr == netmask.s_addr ? prefix : -1;
}

}  // namespace aember::network::netlink
