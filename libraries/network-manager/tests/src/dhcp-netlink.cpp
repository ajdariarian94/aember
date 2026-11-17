/**
 * @file dhcp-netlink.cpp
 * @author Arian Ajdari
 * @brief Tests for the built-in DHCP client and the rtnetlink helpers that
 *        replaced BusyBox udhcpc and `ip`. Packet building/parsing runs
 *        anywhere; the exchange with a fake server on the loopback and the
 *        bridge/address calls need root (they run inside the QEMU VM).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/network-manager/dhcp-client.h>
#include <aember-libs/network-manager/netlink.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// As root, run in a fresh network namespace (only a down `lo`): the bridge,
// veth and DHCP tests then never touch the real network of the VM or the
// dev container. Root tests skip if that isn't possible.
const bool kPrivateNet = ::geteuid() == 0 && ::unshare(CLONE_NEWNET) == 0;

#define AEMBER_REQUIRE_PRIVATE_NET()                                  \
  do {                                                                \
    if (::geteuid() != 0) GTEST_SKIP() << "needs root";               \
    if (!kPrivateNet) GTEST_SKIP() << "no private network namespace"; \
  } while (0)

namespace dhcp = aember::network::dhcp;
namespace netlink = aember::network::netlink;
using namespace std::chrono_literals;

in_addr Ip(const char* s) {
  in_addr a{};
  inet_pton(AF_INET, s, &a);
  return a;
}

std::string Str(in_addr a) {
  char buf[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

const dhcp::Mac kMac{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

/// A server reply built like a real DHCP server would.
std::vector<std::uint8_t> Reply(dhcp::MessageType type, std::uint32_t xid,
                                const dhcp::Mac& mac, in_addr yiaddr,
                                bool with_options = true) {
  std::vector<std::uint8_t> p(236, 0);
  p[0] = 2;  // BOOTREPLY
  p[1] = 1;
  p[2] = 6;
  const std::uint32_t nxid = htonl(xid);
  std::memcpy(&p[4], &nxid, 4);
  std::memcpy(&p[16], &yiaddr, 4);
  std::memcpy(&p[28], mac.data(), 6);
  for (const int b : {99, 130, 83, 99})
    p.push_back(static_cast<std::uint8_t>(b));
  const auto opt = [&](std::uint8_t code, std::vector<std::uint8_t> v) {
    p.push_back(code);
    p.push_back(static_cast<std::uint8_t>(v.size()));
    p.insert(p.end(), v.begin(), v.end());
  };
  opt(53, {static_cast<std::uint8_t>(type)});
  if (with_options) {
    p.push_back(0);                     // pad
    opt(1, {255, 255, 240, 0});         // /20
    opt(3, {10, 0, 2, 2});              // router
    opt(6, {10, 0, 2, 3, 9, 9, 9, 9});  // two DNS servers
    opt(51, {0, 0, 0x0e, 0x10});        // 3600 s
    opt(54, {10, 0, 2, 2});             // server id
    opt(12, {'h', 'o', 's', 't'});      // unknown: skipped
  }
  p.push_back(255);
  return p;
}

/// Value of a DHCP option in a request we built.
std::vector<std::uint8_t> Option(const std::vector<std::uint8_t>& p,
                                 std::uint8_t code) {
  for (std::size_t i = 240; i < p.size();) {
    const auto c = p[i++];
    if (c == 0) continue;
    if (c == 255) break;
    const auto len = p[i++];
    if (c == code) return {p.begin() + i, p.begin() + i + len};
    i += len;
  }
  return {};
}

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------

TEST(DhcpPacketTest, DiscoverLayout) {
  const auto p = dhcp::BuildDiscover(0xdeadbeef, kMac);
  ASSERT_GE(p.size(), 244u);
  EXPECT_EQ(p[0], 1);  // BOOTREQUEST
  EXPECT_EQ(p[1], 1);
  EXPECT_EQ(p[2], 6);
  std::uint32_t xid = 0;
  std::memcpy(&xid, &p[4], 4);
  EXPECT_EQ(ntohl(xid), 0xdeadbeefu);
  EXPECT_EQ(p[10], 0x80);  // broadcast flag: we have no address yet
  EXPECT_EQ(0, std::memcmp(&p[28], kMac.data(), 6));
  EXPECT_EQ(p[236], 99);
  EXPECT_EQ(p[239], 99);
  EXPECT_EQ(Option(p, 53), std::vector<std::uint8_t>{1});  // DISCOVER
  EXPECT_FALSE(Option(p, 55).empty());                     // param list
  EXPECT_EQ(p.back(), 255);
}

TEST(DhcpPacketTest, RequestCarriesAddressAndServer) {
  const auto p = dhcp::BuildRequest(7, kMac, Ip("10.0.2.15"), Ip("10.0.2.2"));
  EXPECT_EQ(Option(p, 53), std::vector<std::uint8_t>{3});  // REQUEST
  EXPECT_EQ(Option(p, 50), (std::vector<std::uint8_t>{10, 0, 2, 15}));
  EXPECT_EQ(Option(p, 54), (std::vector<std::uint8_t>{10, 0, 2, 2}));
}

TEST(DhcpPacketTest, ParseFullOffer) {
  const auto m =
      dhcp::Parse(Reply(dhcp::MessageType::kOffer, 42, kMac, Ip("10.0.2.15")));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->type, dhcp::MessageType::kOffer);
  EXPECT_EQ(m->xid, 42u);
  EXPECT_EQ(m->chaddr, kMac);
  EXPECT_EQ(Str(m->lease.address), "10.0.2.15");
  EXPECT_EQ(Str(m->lease.netmask), "255.255.240.0");
  EXPECT_EQ(m->lease.PrefixLength(), 20);
  ASSERT_TRUE(m->lease.router.has_value());
  EXPECT_EQ(Str(*m->lease.router), "10.0.2.2");
  ASSERT_EQ(m->lease.dns.size(), 2u);
  EXPECT_EQ(Str(m->lease.dns[1]), "9.9.9.9");
  EXPECT_EQ(m->lease.lease_seconds, 3600u);
  EXPECT_EQ(Str(m->lease.server), "10.0.2.2");
}

TEST(DhcpPacketTest, ParseMinimalAck) {
  const auto m = dhcp::Parse(
      Reply(dhcp::MessageType::kAck, 1, kMac, Ip("192.168.1.9"), false));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->type, dhcp::MessageType::kAck);
  EXPECT_FALSE(m->lease.router.has_value());
  EXPECT_TRUE(m->lease.dns.empty());
  EXPECT_EQ(m->lease.PrefixLength(), 24);  // no mask sent: /24
}

TEST(DhcpPacketTest, ParseRejectsGarbage) {
  auto good = Reply(dhcp::MessageType::kOffer, 1, kMac, Ip("10.0.0.2"));
  EXPECT_FALSE(dhcp::Parse({}).has_value());
  EXPECT_FALSE(dhcp::Parse({good.data(), 200}).has_value());  // short

  auto request = good;
  request[0] = 1;  // BOOTREQUEST, not a reply
  EXPECT_FALSE(dhcp::Parse(request).has_value());

  auto cookie = good;
  cookie[236] = 0;
  EXPECT_FALSE(dhcp::Parse(cookie).has_value());

  // Truncated inside the netmask option (type 53 at 240..242, pad at 243,
  // mask option starts at 244 and needs 6 bytes).
  EXPECT_FALSE(dhcp::Parse({good.data(), 247}).has_value());

  // No message type option.
  std::vector<std::uint8_t> no_type(good.begin(), good.begin() + 240);
  no_type.push_back(255);
  EXPECT_FALSE(dhcp::Parse(no_type).has_value());
}

TEST(DhcpPacketTest, PrefixLengthOfOddMasks) {
  dhcp::Lease l;
  l.netmask = Ip("255.255.0.0");
  EXPECT_EQ(l.PrefixLength(), 16);
  l.netmask = Ip("255.0.255.0");  // not contiguous
  EXPECT_EQ(l.PrefixLength(), 24);
}

TEST(DhcpFrameTest, RoundTrip) {
  const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};  // odd length
  const auto frame = dhcp::BuildUdpFrame(
      payload, Ip("0.0.0.0"), Ip("255.255.255.255"), 68, 67);
  ASSERT_EQ(frame.size(), 20u + 8u + payload.size());
  EXPECT_EQ(frame[0], 0x45);
  EXPECT_EQ(frame[9], IPPROTO_UDP);
  const auto back = dhcp::ParseUdpFrame(frame, 67);
  EXPECT_EQ(std::vector<std::uint8_t>(back.begin(), back.end()), payload);
}

TEST(DhcpFrameTest, RejectsWrongPortBadChecksumsAndFragments) {
  const std::vector<std::uint8_t> payload(300, 0xAB);
  const auto frame = dhcp::BuildUdpFrame(
      payload, Ip("10.0.2.2"), Ip("255.255.255.255"), 67, 68);
  EXPECT_FALSE(dhcp::ParseUdpFrame(frame, 68).empty());
  EXPECT_TRUE(dhcp::ParseUdpFrame(frame, 67).empty());  // not for us

  auto bad_udp = frame;
  bad_udp[40] ^= 0xFF;  // payload byte: UDP checksum no longer matches
  EXPECT_TRUE(dhcp::ParseUdpFrame(bad_udp, 68).empty());

  auto no_udp_sum = bad_udp;
  no_udp_sum[26] = no_udp_sum[27] = 0;  // checksum 0 = not used
  EXPECT_FALSE(dhcp::ParseUdpFrame(no_udp_sum, 68).empty());

  auto bad_ip = frame;
  bad_ip[8] = 1;  // TTL changed without fixing the header checksum
  EXPECT_TRUE(dhcp::ParseUdpFrame(bad_ip, 68).empty());

  auto tcp = frame;
  tcp[9] = IPPROTO_TCP;
  EXPECT_TRUE(dhcp::ParseUdpFrame(tcp, 68).empty());

  EXPECT_TRUE(dhcp::ParseUdpFrame({frame.data(), 30}, 68).empty());  // short
}

// ---------------------------------------------------------------------------
// Netlink helpers
// ---------------------------------------------------------------------------

TEST(NetlinkTest, PrefixAndNetmask) {
  EXPECT_EQ(Str(netlink::PrefixToNetmask(24)), "255.255.255.0");
  EXPECT_EQ(Str(netlink::PrefixToNetmask(20)), "255.255.240.0");
  EXPECT_EQ(Str(netlink::PrefixToNetmask(0)), "0.0.0.0");
  EXPECT_EQ(Str(netlink::PrefixToNetmask(32)), "255.255.255.255");
  for (int p = 0; p <= 32; ++p) {
    EXPECT_EQ(netlink::NetmaskToPrefix(netlink::PrefixToNetmask(p)), p) << p;
  }
  EXPECT_EQ(netlink::NetmaskToPrefix(Ip("255.0.255.0")), -1);
}

TEST(NetlinkTest, InterfaceIndex) {
  EXPECT_GT(netlink::InterfaceIndex("lo"), 0);
  EXPECT_EQ(netlink::InterfaceIndex("aember-nope0"), -1);
}

TEST(NetlinkTest, FirstIPv4Address) {
  EXPECT_FALSE(netlink::FirstIPv4Address("aember-nope0").has_value());
  const auto lo = netlink::FirstIPv4Address("lo");
  if (kPrivateNet) {
    EXPECT_FALSE(lo.has_value());  // fresh namespace: lo has no address
  } else {
    ASSERT_TRUE(lo.has_value());
    EXPECT_EQ(Str(*lo), "127.0.0.1");
  }
}

TEST(NetlinkTest, InvalidRequestsAreRefused) {
  EXPECT_EQ(netlink::CreateBridge(""), -EINVAL);
  EXPECT_EQ(netlink::CreateBridge("a-name-much-too-long-for-linux"), -EINVAL);
  EXPECT_EQ(netlink::AddAddress(1, Ip("10.0.0.1"), 33), -EINVAL);
}

TEST(NetlinkTest, WithoutRootTheKernelSaysNo) {
  if (::geteuid() == 0) GTEST_SKIP() << "running as root";
  EXPECT_EQ(netlink::CreateBridge("aembertest0"), -EPERM);
  EXPECT_EQ(netlink::SetLinkUp(netlink::InterfaceIndex("lo")), -EPERM);
}

// As root (inside the VM): create a bridge, address it, bring it up; a
// second create reports EEXIST.
TEST(NetlinkTest, BridgeAddressAndLinkAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  const char* name = "aembertest0";
  ASSERT_EQ(netlink::CreateBridge(name), 0);
  EXPECT_EQ(netlink::CreateBridge(name), -EEXIST);
  const int idx = netlink::InterfaceIndex(name);
  ASSERT_GT(idx, 0);
  EXPECT_EQ(netlink::AddAddress(idx, Ip("10.123.0.1"), 24), 0);
  EXPECT_EQ(netlink::AddAddress(idx, Ip("10.123.0.1"), 24), 0);  // replace
  EXPECT_EQ(netlink::SetLinkUp(idx), 0);
  EXPECT_EQ(netlink::DeleteLink(name), 0);
  EXPECT_EQ(netlink::InterfaceIndex(name), -1);
  EXPECT_EQ(netlink::DeleteLink(name), -ENODEV);
}

TEST(NetlinkTest, VethPairAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  EXPECT_EQ(netlink::CreateVethPair("", "x"), -EINVAL);
  ASSERT_EQ(netlink::CreateVethPair("aemberva0", "aembervb0"), 0);
  EXPECT_GT(netlink::InterfaceIndex("aemberva0"), 0);
  EXPECT_GT(netlink::InterfaceIndex("aembervb0"), 0);
  EXPECT_EQ(netlink::DeleteLink("aemberva0"), 0);
  EXPECT_EQ(netlink::InterfaceIndex("aembervb0"), -1);  // peer goes too
}

// As root: a full DISCOVER/OFFER/REQUEST/ACK against a fake server on the
// other end of a veth pair (lo can't broadcast). Neither end has an address,
// just like eth0 before its first lease.
TEST(DhcpClientTest, FullExchangeWithFakeServerAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  netlink::DeleteLink("aembercli0");
  ASSERT_EQ(netlink::CreateVethPair("aembercli0", "aembersrv0"), 0);
  ASSERT_EQ(netlink::SetLinkUp(netlink::InterfaceIndex("aembercli0")), 0);
  ASSERT_EQ(netlink::SetLinkUp(netlink::InterfaceIndex("aembersrv0")), 0);
  struct Cleanup {
    ~Cleanup() { netlink::DeleteLink("aembercli0"); }  // removes both ends
  } cleanup;

  // The fake server talks raw frames too: in one network namespace the
  // kernel would drop UDP between the two ends as "from a local address".
  const int srv =
      ::socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP));
  ASSERT_GE(srv, 0);
  sockaddr_ll ll{};
  ll.sll_family = AF_PACKET;
  ll.sll_protocol = htons(ETH_P_IP);
  ll.sll_ifindex = netlink::InterfaceIndex("aembersrv0");
  ASSERT_EQ(::bind(srv, reinterpret_cast<sockaddr*>(&ll), sizeof(ll)), 0);
  sockaddr_ll bcast = ll;
  bcast.sll_halen = 6;
  std::memset(bcast.sll_addr, 0xFF, 6);

  std::atomic<bool> stop{false};
  std::atomic<int> received{0};
  std::atomic<int> sent{0};
  std::jthread server{[&] {
    std::array<std::uint8_t, 2048> buf{};
    timeval tv{0, 200'000};
    ::setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!stop) {
      const ssize_t n = ::recv(srv, buf.data(), buf.size(), 0);
      if (n <= 0) continue;
      const auto payload =
          dhcp::ParseUdpFrame({buf.data(), static_cast<std::size_t>(n)}, 67);
      if (payload.size() < 240) continue;
      ++received;
      std::vector<std::uint8_t> req(payload.begin(), payload.end());
      std::uint32_t nxid = 0;
      std::memcpy(&nxid, &req[4], 4);
      dhcp::Mac mac{};
      std::memcpy(mac.data(), &req[28], 6);
      const auto type = Option(req, 53);
      if (type.empty()) continue;
      const auto reply_type =
          type[0] == 1 ? dhcp::MessageType::kOffer : dhcp::MessageType::kAck;
      const auto reply = Reply(reply_type, ntohl(nxid), mac, Ip("10.0.2.15"));
      const auto frame = dhcp::BuildUdpFrame(
          reply, Ip("10.0.2.2"), Ip("255.255.255.255"), 67, 68);
      if (::sendto(srv,
                   frame.data(),
                   frame.size(),
                   0,
                   reinterpret_cast<sockaddr*>(&bcast),
                   sizeof(bcast)) > 0) {
        ++sent;
      }
    }
  }};

  dhcp::DhcpClient client{"aembercli0"};
  const auto lease = client.Acquire(5s);
  stop = true;
  server.join();
  ::close(srv);

  ASSERT_TRUE(lease.has_value()) << lease.error() << " (server received "
                                 << received << ", sent " << sent << ")";
  EXPECT_EQ(Str(lease->address), "10.0.2.15");
  EXPECT_EQ(lease->PrefixLength(), 20);
  EXPECT_EQ(Str(*lease->router), "10.0.2.2");
  EXPECT_EQ(lease->dns.size(), 2u);
}

TEST(DhcpClientTest, UnknownInterfaceFailsFast) {
  dhcp::DhcpClient client{"aember-nope0"};
  const auto t0 = std::chrono::steady_clock::now();
  const auto lease = client.Acquire(3s);
  EXPECT_FALSE(lease.has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

}  // namespace
