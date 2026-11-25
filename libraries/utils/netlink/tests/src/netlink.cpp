/**
 * @file netlink.cpp
 * @author Arian Ajdari
 * @brief Tests for the rtnetlink helpers (they replaced BusyBox `ip`).
 *        Pure helpers run anywhere; bridge/veth/address calls need root and
 *        then run in a private network namespace.
 * @version 0.1
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/netlink/netlink.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sched.h>
#include <unistd.h>

namespace {

// As root, run in a fresh network namespace (only a down `lo`): the bridge
// and veth tests then never touch the real network of the VM or the dev
// container. Root tests skip if that isn't possible.
const bool kPrivateNet = ::geteuid() == 0 && ::unshare(CLONE_NEWNET) == 0;

#define AEMBER_REQUIRE_PRIVATE_NET()                                  \
  do {                                                                \
    if (::geteuid() != 0) GTEST_SKIP() << "needs root";               \
    if (!kPrivateNet) GTEST_SKIP() << "no private network namespace"; \
  } while (0)

namespace netlink = aember::utils::netlink;

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

TEST(NetlinkTest, LinkUsableUnknownInterface) {
  EXPECT_FALSE(netlink::LinkUsable("aember-nope0"));
}

// Regression: the link state came from /sys/class/net, which shows the
// namespace /sys was mounted in (a private namespace never saw its veth's
// carrier), and IFF_RUNNING alone also counted a dormant Wi-Fi link.
TEST(NetlinkTest, LinkUsableNeedsUpAndACarrierAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  ASSERT_EQ(netlink::CreateVethPair("aemberua0", "aemberub0"), 0);
  EXPECT_FALSE(netlink::LinkUsable("aemberua0"));  // down
  ASSERT_EQ(netlink::SetLinkUp(netlink::InterfaceIndex("aemberua0")), 0);
  EXPECT_FALSE(netlink::LinkUsable("aemberua0"));  // up, peer down: no carrier
  ASSERT_EQ(netlink::SetLinkUp(netlink::InterfaceIndex("aemberub0")), 0);
  bool usable = false;
  for (int i = 0; i < 30 && !usable; ++i) {
    usable = netlink::LinkUsable("aemberua0");
    if (!usable) ::usleep(100'000);  // the carrier takes a moment
  }
  EXPECT_TRUE(usable);
  EXPECT_EQ(netlink::DeleteLink("aemberua0"), 0);
}

TEST(NetlinkTest, DeleteAddressAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  const char* name = "aembertest3";
  ASSERT_EQ(netlink::CreateBridge(name), 0);
  const int idx = netlink::InterfaceIndex(name);
  ASSERT_EQ(netlink::AddAddress(idx, Ip("10.125.0.5"), 24), 0);
  EXPECT_EQ(Str(*netlink::FirstIPv4Address(name)), "10.125.0.5");
  EXPECT_EQ(netlink::DeleteAddress(idx, Ip("10.125.0.5"), 24), 0);
  EXPECT_FALSE(netlink::FirstIPv4Address(name).has_value());
  EXPECT_EQ(netlink::DeleteAddress(idx, Ip("10.125.0.5"), 24), -EADDRNOTAVAIL);
  EXPECT_EQ(netlink::DeleteAddress(idx, Ip("10.125.0.5"), 40), -EINVAL);
  EXPECT_EQ(netlink::DeleteLink(name), 0);
}

TEST(NetlinkTest, FirstIPv4Prefix) {
  EXPECT_EQ(netlink::FirstIPv4Prefix("aember-nope0"), -1);
}

TEST(NetlinkTest, FirstIPv4PrefixAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  const char* name = "aembertest4";
  ASSERT_EQ(netlink::CreateBridge(name), 0);
  const int idx = netlink::InterfaceIndex(name);
  EXPECT_EQ(netlink::FirstIPv4Prefix(name), -1);
  ASSERT_EQ(netlink::AddAddress(idx, Ip("10.126.0.5"), 20), 0);
  EXPECT_EQ(netlink::FirstIPv4Prefix(name), 20);
  EXPECT_EQ(netlink::DeleteLink(name), 0);
}

TEST(NetlinkTest, ParseCidr) {
  in_addr addr{};
  int prefix = -1;
  ASSERT_TRUE(netlink::ParseCidr("10.0.3.1/24", addr, prefix));
  EXPECT_EQ(Str(addr), "10.0.3.1");
  EXPECT_EQ(prefix, 24);
  EXPECT_TRUE(netlink::ParseCidr("0.0.0.0/0", addr, prefix));
  EXPECT_TRUE(netlink::ParseCidr("1.2.3.4/32", addr, prefix));
  for (const char* bad : {"10.0.3.1",
                          "10.0.3.1/",
                          "10.0.3.1/33",
                          "10.0.3.1/-1",
                          "10.0.3.1/2x",
                          "10.0.3/24",
                          "host/24",
                          "",
                          "/24"}) {
    EXPECT_FALSE(netlink::ParseCidr(bad, addr, prefix)) << bad;
  }
}

TEST(NetlinkTest, EnsureBridgeRejectsBadCidr) {
  EXPECT_EQ(netlink::EnsureBridge("aembertest1", "10.0.0.1/99"), -EINVAL);
  EXPECT_EQ(netlink::InterfaceIndex("aembertest1"), -1);  // nothing created
}

// Idempotent: a restarted containerd finds the bridge already there.
// A bridge that can't be created (no root: EPERM; a name longer than
// IFNAMSIZ: EINVAL) is reported as that error, not as a missing device.
TEST(NetlinkTest, EnsureBridgeReportsTheCreateError) {
  const int rc = netlink::EnsureBridge("aember-name-too-long-for-a-link", "");
  EXPECT_LT(rc, 0);
  EXPECT_NE(rc, -ENODEV);
}

// The broadcast address is set below /31 only (RFC 3021 point-to-point).
TEST(NetlinkTest, BroadcastOnlyBelowSlash31AsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  const char* name = "aembertest3";
  ASSERT_EQ(netlink::CreateBridge(name), 0);
  const int idx = netlink::InterfaceIndex(name);
  ASSERT_GT(idx, 0);
  ASSERT_EQ(netlink::AddAddress(idx, Ip("10.125.0.1"), 24), 0);
  ASSERT_EQ(netlink::AddAddress(idx, Ip("10.126.0.1"), 32), 0);
  std::string brd24 = "none";
  std::string brd32 = "none";
  ifaddrs* list = nullptr;
  ASSERT_EQ(::getifaddrs(&list), 0);
  for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
    if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET ||
        std::string{a->ifa_name} != name) {
      continue;
    }
    const auto local =
        Str(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr);
    std::string brd = "none";
    if (a->ifa_broadaddr != nullptr) {
      brd = Str(reinterpret_cast<sockaddr_in*>(a->ifa_broadaddr)->sin_addr);
    }
    (local == "10.125.0.1" ? brd24 : brd32) = brd;
  }
  ::freeifaddrs(list);
  EXPECT_EQ(brd24, "10.125.0.255");
  EXPECT_NE(brd32, "10.126.0.255");
  EXPECT_EQ(netlink::DeleteLink(name), 0);
}

TEST(NetlinkTest, EnsureBridgeAsRoot) {
  AEMBER_REQUIRE_PRIVATE_NET();
  const char* name = "aembertest2";
  ASSERT_EQ(netlink::EnsureBridge(name, "10.124.0.1/24"), 0);
  ASSERT_EQ(netlink::EnsureBridge(name, "10.124.0.1/24"), 0);
  const auto addr = netlink::FirstIPv4Address(name);
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(Str(*addr), "10.124.0.1");
  EXPECT_EQ(netlink::DeleteLink(name), 0);
}

}  // namespace
