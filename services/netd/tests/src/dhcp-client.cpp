/**
 * @file dhcp-client.cpp
 * @author Arian Ajdari
 * @brief Tests for netd's built-in DHCP client (it replaced BusyBox
 *        udhcpc). Packet building/parsing runs anywhere; the exchange with
 *        a fake server over a veth pair needs root (it runs inside the QEMU
 *        VM, or in the dev container as root).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/netlink/netlink.h>
#include <aember-services/netd/dhcp-client.h>
#include <aember-services/netd/network-manager.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// As root, run in a fresh network namespace (only a down `lo`): the bridge,
// veth and DHCP tests then never touch the real network of the VM or the
// dev container. A private mount namespace with a temp file over
// /etc/resolv.conf keeps NetworkManager from rewriting the real one. Root
// tests skip if that isn't possible.
bool EnterPrivateNamespaces() {
  if (::geteuid() != 0) return false;
  if (::unshare(CLONE_NEWNET | CLONE_NEWNS) != 0) return false;
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    return false;
  }
  const std::string fake =
      "/tmp/aember-test-resolv.conf." + std::to_string(::getpid());
  { std::ofstream{fake}; }
  const bool ok =
      ::mount(fake.c_str(), "/etc/resolv.conf", nullptr, MS_BIND, nullptr) == 0;
  ::unlink(fake.c_str());  // the bind mount keeps it alive while we run
  return ok;
}
const bool kPrivateNet = EnterPrivateNamespaces();

#define AEMBER_REQUIRE_PRIVATE_NET()                                  \
  do {                                                                \
    if (::geteuid() != 0) GTEST_SKIP() << "needs root";               \
    if (!kPrivateNet) GTEST_SKIP() << "no private network namespace"; \
  } while (0)

namespace dhcp = aember::netd::dhcp;
namespace netlink = aember::utils::netlink;
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
                                bool with_options = true,
                                std::uint32_t lease_seconds = 3600) {
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
    const auto be = [](std::uint32_t v) {
      return std::vector<std::uint8_t>{static_cast<std::uint8_t>(v >> 24),
                                       static_cast<std::uint8_t>(v >> 16),
                                       static_cast<std::uint8_t>(v >> 8),
                                       static_cast<std::uint8_t>(v)};
    };
    opt(51, be(lease_seconds));     // 3600 s unless asked otherwise
    opt(54, {10, 0, 2, 2});         // server id
    opt(12, {'h', 'o', 's', 't'});  // unknown: skipped
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

// ---------------------------------------------------------------------------
// Renewal: packets and lease timers
// ---------------------------------------------------------------------------

TEST(DhcpRenewTest, RenewLayout) {
  const auto p = dhcp::BuildRenew(9, kMac, Ip("10.0.2.15"));
  EXPECT_EQ(p[0], 1);   // BOOTREQUEST
  EXPECT_EQ(p[10], 0);  // no broadcast flag: unicast reply
  EXPECT_EQ(p[11], 0);
  in_addr ciaddr{};
  std::memcpy(&ciaddr, &p[12], 4);
  EXPECT_EQ(Str(ciaddr), "10.0.2.15");
  EXPECT_EQ(Option(p, 53), std::vector<std::uint8_t>{3});  // REQUEST
  EXPECT_TRUE(Option(p, 50).empty());  // RFC 2131: no requested address
  EXPECT_TRUE(Option(p, 54).empty());  // ... and no server id
}

TEST(DhcpRenewTest, InitRebootLayout) {
  const auto p = dhcp::BuildInitReboot(9, kMac, Ip("10.0.2.15"));
  in_addr ciaddr{};
  std::memcpy(&ciaddr, &p[12], 4);
  EXPECT_EQ(ciaddr.s_addr, 0u);
  EXPECT_EQ(Option(p, 53), std::vector<std::uint8_t>{3});
  EXPECT_EQ(Option(p, 50), (std::vector<std::uint8_t>{10, 0, 2, 15}));
  EXPECT_TRUE(Option(p, 54).empty());
}

TEST(DhcpRenewTest, ParsesT1AndT2) {
  auto r = Reply(dhcp::MessageType::kAck, 1, kMac, Ip("10.0.2.15"));
  r.pop_back();                                                  // END
  for (const int b : {58, 4, 0, 0, 0x07, 0x08}) r.push_back(b);  // 1800
  for (const int b : {59, 4, 0, 0, 0x0c, 0x4e}) r.push_back(b);  // 3150
  r.push_back(255);
  const auto m = dhcp::Parse(r);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->lease.lease_seconds, 3600u);
  EXPECT_EQ(m->lease.renew_seconds, 1800u);
  EXPECT_EQ(m->lease.rebind_seconds, 3150u);
}

TEST(LeaseTimerTest, DefaultsAreHalfAndSevenEighths) {
  using Phase = dhcp::LeaseTimer::Phase;
  dhcp::Lease l;
  l.lease_seconds = 800;
  const auto t0 = dhcp::LeaseTimer::Clock::time_point{} + 1000s;
  const dhcp::LeaseTimer timer{l, t0};
  EXPECT_EQ(timer.RenewAt(), t0 + 400s);
  EXPECT_EQ(timer.RebindAt(), t0 + 700s);
  EXPECT_EQ(timer.ExpiresAt(), t0 + 800s);
  EXPECT_EQ(timer.At(t0), Phase::kBound);
  EXPECT_EQ(timer.At(t0 + 399s), Phase::kBound);
  EXPECT_EQ(timer.At(t0 + 400s), Phase::kRenewing);
  EXPECT_EQ(timer.At(t0 + 700s), Phase::kRebinding);
  EXPECT_EQ(timer.At(t0 + 800s), Phase::kExpired);
}

TEST(LeaseTimerTest, ServerTimesAndNonsense) {
  dhcp::Lease l;
  l.lease_seconds = 1000;
  l.renew_seconds = 100;
  l.rebind_seconds = 200;
  const auto t0 = dhcp::LeaseTimer::Clock::time_point{};
  dhcp::LeaseTimer timer{l, t0};
  EXPECT_EQ(timer.RenewAt(), t0 + 100s);
  EXPECT_EQ(timer.RebindAt(), t0 + 200s);

  l.renew_seconds = 5000;   // after the lease ends
  l.rebind_seconds = 4000;  // and after T1's place
  timer = dhcp::LeaseTimer{l, t0};
  EXPECT_LE(timer.RenewAt(), timer.RebindAt());
  EXPECT_LE(timer.RebindAt(), timer.ExpiresAt());
  EXPECT_EQ(timer.RebindAt(), t0 + 875s);
  EXPECT_EQ(timer.RenewAt(), t0 + 500s);
}

TEST(LeaseTimerTest, InfiniteLeaseNeverRenews) {
  dhcp::Lease l;
  l.lease_seconds = 0xFFFFFFFFu;
  const dhcp::LeaseTimer timer{l, {}};
  EXPECT_EQ(timer.At(dhcp::LeaseTimer::Clock::time_point{} + 1000000h),
            dhcp::LeaseTimer::Phase::kInfinite);
}

// ---------------------------------------------------------------------------
// Renewal against a fake server (root, private network namespace)
// ---------------------------------------------------------------------------

/// UDP payload of an IPv4 frame to @p port, without checking the UDP
/// checksum: a packet the local stack sent over veth still has a partial
/// (offloaded) checksum when the peer's AF_PACKET socket sees it.
std::vector<std::uint8_t> UdpPayloadToPort(std::span<const std::uint8_t> f,
                                           std::uint16_t port) {
  if (f.size() < 28 || (f[0] >> 4) != 4 || f[9] != IPPROTO_UDP) return {};
  const std::size_t ihl = (f[0] & 0x0F) * 4u;
  if (f.size() < ihl + 8) return {};
  if (((f[ihl + 2] << 8) | f[ihl + 3]) != port) return {};
  return {f.begin() + static_cast<std::ptrdiff_t>(ihl + 8), f.end()};
}

/// A raw-frame DHCP server on the "srv" end of a veth pair. @p answer gets
/// the request type and returns the reply type (or nullopt: stay silent).
class FakeDhcpServer {
 public:
  using Policy =
      std::function<std::optional<dhcp::MessageType>(std::uint8_t type)>;

  explicit FakeDhcpServer(Policy answer, std::uint32_t lease_seconds = 3600)
      : answer_(std::move(answer)), lease_seconds_(lease_seconds) {
    netlink::DeleteLink("aembercli1");
    ok_ = netlink::CreateVethPair("aembercli1", "aembersrv1") == 0 &&
          netlink::SetLinkUp(netlink::InterfaceIndex("aembercli1")) == 0 &&
          netlink::SetLinkUp(netlink::InterfaceIndex("aembersrv1")) == 0;
    fd_ = ::socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP));
    sockaddr_ll ll{};
    ll.sll_family = AF_PACKET;
    ll.sll_protocol = htons(ETH_P_IP);
    ll.sll_ifindex = netlink::InterfaceIndex("aembersrv1");
    ok_ = ok_ && fd_ >= 0 &&
          ::bind(fd_, reinterpret_cast<sockaddr*>(&ll), sizeof(ll)) == 0;
    bcast_ = ll;
    bcast_.sll_halen = 6;
    std::memset(bcast_.sll_addr, 0xFF, 6);
    thread_ = std::jthread{[this](std::stop_token st) { Loop(st); }};
  }
  ~FakeDhcpServer() {
    thread_ = {};
    if (fd_ >= 0) ::close(fd_);
    netlink::DeleteLink("aembercli1");
  }
  [[nodiscard]] bool ok() const { return ok_; }
  /// ciaddr of the last REQUEST, option 50 of it.
  std::atomic<std::uint32_t> last_ciaddr{0};
  std::atomic<bool> last_had_requested_ip{false};
  std::atomic<int> requests{0};

 private:
  void Loop(std::stop_token st) {
    std::array<std::uint8_t, 2048> buf{};
    timeval tv{0, 100'000};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!st.stop_requested()) {
      const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
      if (n <= 0) continue;
      auto req =
          UdpPayloadToPort({buf.data(), static_cast<std::size_t>(n)}, 67);
      if (req.size() < 240) continue;
      const auto type = Option(req, 53);
      if (type.empty()) continue;
      ++requests;
      std::uint32_t ci = 0;
      std::memcpy(&ci, &req[12], 4);
      last_ciaddr = ci;
      last_had_requested_ip = !Option(req, 50).empty();
      const auto reply_type = answer_(type[0]);
      if (!reply_type) continue;
      std::uint32_t nxid = 0;
      std::memcpy(&nxid, &req[4], 4);
      dhcp::Mac mac{};
      std::memcpy(mac.data(), &req[28], 6);
      const auto reply = Reply(
          *reply_type, ntohl(nxid), mac, Ip("10.0.2.15"), true, lease_seconds_);
      const auto frame = dhcp::BuildUdpFrame(
          reply, Ip("10.0.2.2"), Ip("255.255.255.255"), 67, 68);
      ::sendto(fd_,
               frame.data(),
               frame.size(),
               0,
               reinterpret_cast<sockaddr*>(&bcast_),
               sizeof(bcast_));
    }
  }

  Policy answer_;
  std::uint32_t lease_seconds_;
  int fd_ = -1;
  bool ok_ = false;
  sockaddr_ll bcast_{};
  std::jthread thread_;
};

dhcp::Lease BoundLease() {
  dhcp::Lease l;
  l.address = Ip("10.0.2.15");
  l.netmask = Ip("255.255.240.0");
  l.server = Ip("10.0.2.2");
  l.router = Ip("10.0.2.2");
  l.lease_seconds = 60;
  return l;
}

// Rebinding: a broadcast REQUEST from the bound address over a UDP socket.
TEST(DhcpClientTest, RebindAgainstFakeServerAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  FakeDhcpServer server{[](std::uint8_t) { return dhcp::MessageType::kAck; }};
  ASSERT_TRUE(server.ok());
  const int idx = netlink::InterfaceIndex("aembercli1");
  ASSERT_EQ(netlink::AddAddress(idx, Ip("10.0.2.15"), 20), 0);

  dhcp::DhcpClient client{"aembercli1"};
  const auto lease = client.Renew(BoundLease(), /*broadcast=*/true, 5s);
  ASSERT_TRUE(lease.has_value())
      << lease.error().message << " (requests " << server.requests << ")";
  EXPECT_EQ(Str(lease->address), "10.0.2.15");
  EXPECT_EQ(lease->lease_seconds, 3600u);  // the server's new lease
  in_addr ci{server.last_ciaddr.load()};
  EXPECT_EQ(Str(ci), "10.0.2.15");  // ciaddr carries the bound address
  EXPECT_FALSE(server.last_had_requested_ip.load());
}

TEST(DhcpClientTest, RenewNakIsReportedAsNakAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  FakeDhcpServer server{[](std::uint8_t) { return dhcp::MessageType::kNak; }};
  ASSERT_TRUE(server.ok());
  ASSERT_EQ(netlink::AddAddress(
                netlink::InterfaceIndex("aembercli1"), Ip("10.0.2.15"), 20),
            0);
  dhcp::DhcpClient client{"aembercli1"};
  const auto lease = client.Renew(BoundLease(), true, 5s);
  ASSERT_FALSE(lease.has_value());
  EXPECT_TRUE(lease.error().nak);
}

TEST(DhcpClientTest, RenewWithoutServerTimesOutAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  FakeDhcpServer server{[](std::uint8_t) { return std::nullopt; }};
  ASSERT_TRUE(server.ok());
  ASSERT_EQ(netlink::AddAddress(
                netlink::InterfaceIndex("aembercli1"), Ip("10.0.2.15"), 20),
            0);
  dhcp::DhcpClient client{"aembercli1"};
  const auto t0 = std::chrono::steady_clock::now();
  const auto lease = client.Renew(BoundLease(), true, 1500ms);
  ASSERT_FALSE(lease.has_value());
  EXPECT_FALSE(lease.error().nak);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s);
  EXPECT_GE(server.requests.load(), 1);
}

// INIT-REBOOT after a netd restart: ACK keeps the address, NAK refuses it.
TEST(DhcpClientTest, ConfirmAgainstFakeServerAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  {
    FakeDhcpServer server{[](std::uint8_t) { return dhcp::MessageType::kAck; }};
    ASSERT_TRUE(server.ok());
    dhcp::DhcpClient client{"aembercli1"};
    const auto lease = client.Confirm(Ip("10.0.2.15"), 5s);
    ASSERT_TRUE(lease.has_value()) << lease.error().message;
    EXPECT_EQ(lease->lease_seconds, 3600u);
    EXPECT_TRUE(server.last_had_requested_ip.load());
    EXPECT_EQ(server.last_ciaddr.load(), 0u);
  }
  {
    FakeDhcpServer server{[](std::uint8_t) { return dhcp::MessageType::kNak; }};
    ASSERT_TRUE(server.ok());
    dhcp::DhcpClient client{"aembercli1"};
    const auto lease = client.Confirm(Ip("10.0.2.15"), 5s);
    ASSERT_FALSE(lease.has_value());
    EXPECT_TRUE(lease.error().nak);
  }
}

// NetworkManager keeps the lease: rebinds after T2, starts over on a NAK.
// A 4 s lease (T1 2 s, T2 3.5 s); MaintainLeases is called by hand.
TEST(DhcpLeaseMaintenanceTest, RebindThenNakAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  std::atomic<bool> refuse{false};
  FakeDhcpServer server{
      [&](std::uint8_t type) -> std::optional<dhcp::MessageType> {
        if (refuse) return dhcp::MessageType::kNak;
        return type == 1 ? dhcp::MessageType::kOffer : dhcp::MessageType::kAck;
      },
      4};
  ASSERT_TRUE(server.ok());
  aember::netd::NetworkManager nm{nlohmann::json::parse(R"({
    "ping_target": "127.0.0.1", "ping_interval_ms": 60000,
    "dhcp_timeout_ms": 3000, "dhcp_retries": 1,
    "interfaces": [{"name": "aembercli1", "mode": "dhcp", "required": true}]
  })")};
  nm.Start();
  ASSERT_TRUE(netlink::FirstIPv4Address("aembercli1").has_value());
  const int initial = server.requests.load();

  nm.MaintainLeases();  // bound: nothing to do
  EXPECT_EQ(server.requests.load(), initial);

  std::this_thread::sleep_for(3600ms);  // past T2: rebinding (broadcast)
  nm.MaintainLeases();
  EXPECT_GT(server.requests.load(), initial);
  in_addr ci{server.last_ciaddr.load()};
  EXPECT_EQ(Str(ci), "10.0.2.15");
  EXPECT_TRUE(netlink::FirstIPv4Address("aembercli1").has_value());

  refuse = true;
  std::this_thread::sleep_for(3600ms);  // past T2 of the renewed lease
  nm.MaintainLeases();  // NAK: the address goes, DISCOVER gets refused too
  EXPECT_FALSE(netlink::FirstIPv4Address("aembercli1").has_value());
  nm.Stop();
}

}  // namespace
