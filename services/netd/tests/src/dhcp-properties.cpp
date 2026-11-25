/**
 * @file dhcp-properties.cpp
 * @author Arian Ajdari
 * @brief Property tests for netd's DHCP packets: server replies with any
 *        option order, padding and unknown options parse to what they say;
 *        cut-off replies never crash; IPv4/UDP frames round trip and a
 *        changed byte is caught; our requests are well-formed; the lease
 *        timer's phases come in order.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/netd/dhcp-client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include <arpa/inet.h>

#include <aember-test/property.h>

namespace {

namespace dhcp = aember::netd::dhcp;
using aember::test::Gen;
using Bytes = std::vector<std::uint8_t>;

in_addr AnyAddress(Gen& g) {
  in_addr a{};
  a.s_addr = g.Int<std::uint32_t>(0, UINT32_MAX);
  return a;
}

void Put(Bytes& p, const void* data, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(data);
  p.insert(p.end(), b, b + n);
}

Bytes Be32(std::uint32_t v) {
  const std::uint32_t n = htonl(v);
  Bytes b(4);
  std::memcpy(b.data(), &n, 4);
  return b;
}

Bytes Addresses(const std::vector<in_addr>& list) {
  Bytes b;
  for (const auto& a : list) Put(b, &a, 4);
  return b;
}

/// A random server reply and the message it means.
std::pair<Bytes, dhcp::Message> RandomReply(Gen& g) {
  dhcp::Message m;
  m.type = g.Pick(std::vector<dhcp::MessageType>{dhcp::MessageType::kOffer,
                                                 dhcp::MessageType::kAck,
                                                 dhcp::MessageType::kNak});
  m.xid = g.Int<std::uint32_t>(0, UINT32_MAX);
  for (auto& b : m.chaddr) b = g.Int<std::uint8_t>(0, 255);
  m.lease.address = AnyAddress(g);

  Bytes p(236, 0);
  p[0] = 2;  // BOOTREPLY
  p[1] = 1;
  p[2] = 6;
  const std::uint32_t nxid = htonl(m.xid);
  std::memcpy(&p[4], &nxid, 4);
  std::memcpy(&p[16], &m.lease.address, 4);
  std::memcpy(&p[28], m.chaddr.data(), m.chaddr.size());
  for (const int b : {99, 130, 83, 99})
    p.push_back(static_cast<std::uint8_t>(b));

  std::vector<std::pair<std::uint8_t, Bytes>> options;
  options.emplace_back(53, Bytes{static_cast<std::uint8_t>(m.type)});
  if (g.Chance(0.7)) {
    m.lease.netmask = AnyAddress(g);
    options.emplace_back(1, Addresses({m.lease.netmask}));
  }
  if (g.Chance(0.7)) {  // one or more routers: the first one counts
    std::vector<in_addr> routers;
    for (int i = g.Int(1, 3); i > 0; --i) routers.push_back(AnyAddress(g));
    m.lease.router = routers.front();
    options.emplace_back(3, Addresses(routers));
  }
  if (g.Chance(0.7)) {  // DNS servers, sometimes split over two options
    for (int i = g.Int(1, 4); i > 0; --i) m.lease.dns.push_back(AnyAddress(g));
    const auto split = g.Int<std::size_t>(0, m.lease.dns.size());
    if (split > 0 && split < m.lease.dns.size() && g.Chance(0.5)) {
      options.emplace_back(6,
                           Addresses({m.lease.dns.begin(),
                                      m.lease.dns.begin() +
                                          static_cast<std::ptrdiff_t>(split)}));
      options.emplace_back(
          6,
          Addresses({m.lease.dns.begin() + static_cast<std::ptrdiff_t>(split),
                     m.lease.dns.end()}));
    } else {
      options.emplace_back(6, Addresses(m.lease.dns));
    }
  }
  if (g.Chance(0.7)) {
    m.lease.lease_seconds = g.Int<std::uint32_t>(0, UINT32_MAX);
    options.emplace_back(51, Be32(m.lease.lease_seconds));
  }
  if (g.Chance(0.7)) {
    m.lease.server = AnyAddress(g);
    options.emplace_back(54, Addresses({m.lease.server}));
  }
  if (g.Chance(0.3)) {
    m.lease.renew_seconds = g.Int<std::uint32_t>(0, UINT32_MAX);
    options.emplace_back(58, Be32(m.lease.renew_seconds));
  }
  if (g.Chance(0.3)) {
    m.lease.rebind_seconds = g.Int<std::uint32_t>(0, UINT32_MAX);
    options.emplace_back(59, Be32(m.lease.rebind_seconds));
  }
  for (int i = g.Size(3); i > 0; --i) {  // options we don't use: skipped
    const auto code = g.Pick(Bytes{2, 12, 15, 28, 42, 119, 121, 252});
    Bytes data;
    for (int k = g.Size(20); k > 0; --k)
      data.push_back(g.Int<std::uint8_t>(0, 255));
    options.emplace_back(code, data);
  }
  // Any order, but the DNS parts keep theirs (they are concatenated).
  std::vector<std::size_t> idx(options.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::ranges::shuffle(idx, g.Engine());
  std::vector<std::size_t> dns_slots;
  for (const auto i : idx) {
    if (options[i].first == 6) dns_slots.push_back(i);
  }
  std::ranges::sort(dns_slots);
  std::size_t next_dns = 0;
  for (const auto i : idx) {
    const auto& [code, data] =
        options[i].first == 6 ? options[dns_slots[next_dns++]] : options[i];
    while (g.Chance(0.2)) p.push_back(0);  // pad
    p.push_back(code);
    p.push_back(static_cast<std::uint8_t>(data.size()));
    p.insert(p.end(), data.begin(), data.end());
  }
  if (g.Chance(0.8)) {
    p.push_back(255);  // end; what follows is ignored
    for (int k = g.Size(10); k > 0; --k)
      p.push_back(g.Int<std::uint8_t>(0, 255));
  }
  return {p, m};
}

TEST(DhcpProperty, RepliesParseToWhatTheySay) {
  aember::test::ForAll(1000, [](Gen& g) {
    const auto [packet, want] = RandomReply(g);
    const auto got = dhcp::Parse(packet);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->type, want.type);
    EXPECT_EQ(got->xid, want.xid);
    EXPECT_EQ(got->chaddr, want.chaddr);
    EXPECT_EQ(got->lease.address.s_addr, want.lease.address.s_addr);
    EXPECT_EQ(got->lease.netmask.s_addr, want.lease.netmask.s_addr);
    EXPECT_EQ(got->lease.router.has_value(), want.lease.router.has_value());
    if (got->lease.router && want.lease.router) {
      EXPECT_EQ(got->lease.router->s_addr, want.lease.router->s_addr);
    }
    ASSERT_EQ(got->lease.dns.size(), want.lease.dns.size());
    for (std::size_t i = 0; i < want.lease.dns.size(); ++i) {
      EXPECT_EQ(got->lease.dns[i].s_addr, want.lease.dns[i].s_addr);
    }
    EXPECT_EQ(got->lease.server.s_addr, want.lease.server.s_addr);
    EXPECT_EQ(got->lease.lease_seconds, want.lease.lease_seconds);
    EXPECT_EQ(got->lease.renew_seconds, want.lease.renew_seconds);
    EXPECT_EQ(got->lease.rebind_seconds, want.lease.rebind_seconds);
  });
}

TEST(DhcpProperty, CutOffOrChangedRepliesNeverCrash) {
  aember::test::ForAll(1000, [](Gen& g) {
    auto packet = RandomReply(g).first;
    if (g.Chance(0.5)) {
      packet.resize(g.Int<std::size_t>(0, packet.size()));
    } else {
      for (int i = g.Int(1, 4); i > 0; --i) {
        packet[g.Int<std::size_t>(0, packet.size() - 1)] =
            g.Int<std::uint8_t>(0, 255);
      }
    }
    std::ignore = dhcp::Parse(packet);  // ASan/valgrind: no read past the end
  });
}

TEST(DhcpProperty, UdpFramesRoundTripAndChangesAreCaught) {
  aember::test::ForAll(1000, [](Gen& g) {
    Bytes payload;
    for (int i = g.Size(600); i > 0; --i)
      payload.push_back(g.Int<std::uint8_t>(0, 255));
    const auto src = AnyAddress(g);
    const auto dst = AnyAddress(g);
    const auto sport = g.Int<std::uint16_t>(0, 65535);
    const auto dport = g.Int<std::uint16_t>(1, 65535);
    auto frame = dhcp::BuildUdpFrame(payload, src, dst, sport, dport);
    const auto got = dhcp::ParseUdpFrame(frame, dport);
    EXPECT_TRUE(std::ranges::equal(got, payload));
    EXPECT_TRUE(
        dhcp::ParseUdpFrame(frame, static_cast<std::uint16_t>(dport - 1))
            .empty());
    // One changed byte: refused, or (a changed UDP checksum byte that turned
    // it into 0 = "no checksum") the same payload.
    const auto at = g.Int<std::size_t>(0, frame.size() - 1);
    frame[at] = static_cast<std::uint8_t>(frame[at] ^ g.Int(1, 255));
    const auto changed = dhcp::ParseUdpFrame(frame, dport);
    EXPECT_TRUE(changed.empty() || std::ranges::equal(changed, payload))
        << "byte " << at;
  });
}

/// The options of one of our requests (cookie checked), by code.
std::optional<std::map<int, Bytes>> Options(const Bytes& p) {
  if (p.size() < 241 || p[0] != 1 || p[236] != 99 || p[237] != 130 ||
      p[238] != 83 || p[239] != 99) {
    return std::nullopt;
  }
  std::map<int, Bytes> out;
  for (std::size_t i = 240; i < p.size();) {
    const int code = p[i++];
    if (code == 0) continue;
    if (code == 255) return i == p.size() ? std::optional{out} : std::nullopt;
    if (i >= p.size() || i + 1 + p[i] > p.size()) return std::nullopt;
    const std::size_t len = p[i++];
    if (out.contains(code)) return std::nullopt;  // each option once
    out[code] = Bytes(p.begin() + static_cast<std::ptrdiff_t>(i),
                      p.begin() + static_cast<std::ptrdiff_t>(i + len));
    i += len;
  }
  return std::nullopt;  // no end option
}

TEST(DhcpProperty, OurRequestsAreWellFormed) {
  aember::test::ForAll(300, [](Gen& g) {
    const auto xid = g.Int<std::uint32_t>(0, UINT32_MAX);
    dhcp::Mac mac{};
    for (auto& b : mac) b = g.Int<std::uint8_t>(0, 255);
    const auto a = AnyAddress(g);
    const auto server = AnyAddress(g);
    const auto check = [&](const Bytes& p,
                           std::uint8_t type) -> std::map<int, Bytes> {
      const auto opts = Options(p);
      EXPECT_TRUE(opts.has_value());
      if (!opts) return {{53, {}}, {50, {}}, {54, {}}};
      std::uint32_t nxid = 0;
      std::memcpy(&nxid, &p[4], 4);
      EXPECT_EQ(ntohl(nxid), xid);
      EXPECT_TRUE(std::equal(mac.begin(), mac.end(), p.begin() + 28));
      EXPECT_EQ(opts->at(53), Bytes{type});
      EXPECT_TRUE(opts->contains(55));  // what we ask for
      return *opts;
    };
    check(dhcp::BuildDiscover(xid, mac), 1);
    const auto req = check(dhcp::BuildRequest(xid, mac, a, server), 3);
    EXPECT_EQ(req.at(50), Addresses({a}));
    EXPECT_EQ(req.at(54), Addresses({server}));
    const auto renew = dhcp::BuildRenew(xid, mac, a);
    const auto r = check(renew, 3);
    EXPECT_FALSE(r.contains(50) || r.contains(54));  // RFC 2131 4.3.2
    EXPECT_EQ(std::memcmp(&renew[12], &a, 4), 0);    // ciaddr
    EXPECT_EQ(renew[10] & 0x80, 0);                  // no broadcast flag
    const auto reboot = check(dhcp::BuildInitReboot(xid, mac, a), 3);
    EXPECT_EQ(reboot.at(50), Addresses({a}));
    EXPECT_FALSE(reboot.contains(54));
  });
}

TEST(DhcpProperty, LeasePhasesComeInOrder) {
  using Clock = dhcp::LeaseTimer::Clock;
  using Phase = dhcp::LeaseTimer::Phase;
  aember::test::ForAll(1000, [](Gen& g) {
    dhcp::Lease lease;
    const auto any = [&] {
      return g.Chance(0.3) ? g.Pick(std::vector<std::uint32_t>{
                                 0, 1, 2, UINT32_MAX - 1, UINT32_MAX})
                           : g.Int<std::uint32_t>(0, 100000);
    };
    lease.lease_seconds = any();
    lease.renew_seconds = g.Chance(0.5) ? any() : 0;
    lease.rebind_seconds = g.Chance(0.5) ? any() : 0;
    const auto t0 = Clock::now();
    const dhcp::LeaseTimer timer{lease, t0};
    if (lease.lease_seconds == UINT32_MAX) {
      EXPECT_EQ(timer.At(t0 + std::chrono::hours{24 * 365 * 200}),
                Phase::kInfinite);
      return;
    }
    EXPECT_LE(t0, timer.RenewAt());
    EXPECT_LE(timer.RenewAt(), timer.RebindAt());
    EXPECT_LE(timer.RebindAt(), timer.ExpiresAt());
    EXPECT_EQ(timer.ExpiresAt() - t0,
              std::chrono::seconds{lease.lease_seconds});
    // The phase never goes back as time passes.
    int last = 0;
    for (const auto t : {t0,
                         timer.RenewAt(),
                         timer.RebindAt(),
                         timer.ExpiresAt(),
                         timer.ExpiresAt() + std::chrono::seconds{1}}) {
      const int phase = static_cast<int>(timer.At(t));
      EXPECT_GE(phase, last);
      last = phase;
    }
    EXPECT_EQ(timer.At(timer.ExpiresAt()), Phase::kExpired);
  });
}

}  // namespace
