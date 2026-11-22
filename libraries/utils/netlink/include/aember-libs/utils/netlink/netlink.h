/**
 * @file netlink.h
 * @author Arian Ajdari
 * @brief Minimal rtnetlink client: what aember used to run `ip` (BusyBox)
 *        for, as direct kernel requests.
 *
 *        Each call opens a NETLINK_ROUTE socket, sends one request with
 *        NLM_F_ACK and returns the kernel's answer (0 or -errno).
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <optional>
#include <string_view>

#include <netinet/in.h>

namespace aember::utils::netlink {

/// Interface index, or -1 if there is no such interface.
int InterfaceIndex(std::string_view name);

/// First IPv4 address of interface @p name, if it has one.
std::optional<in_addr> FirstIPv4Address(std::string_view name);

/// The link of @p name can carry traffic: up, a carrier (IFF_LOWER_UP),
/// and not dormant (Wi-Fi before the WPA handshake). False for an unknown
/// interface.
bool LinkUsable(std::string_view name);

/// Prefix length of that first IPv4 address, -1 if there is none.
int FirstIPv4Prefix(std::string_view name);

/// `ip link set <idx> up`
int SetLinkUp(int ifindex);

/// `ip link set <idx> down`
int SetLinkDown(int ifindex);

/// `ip addr replace <addr>/<prefix> dev <idx>` (also sets the broadcast).
int AddAddress(int ifindex, in_addr addr, int prefix_len);

/// `ip addr del <addr>/<prefix> dev <idx>`; -EADDRNOTAVAIL if it isn't
/// there.
int DeleteAddress(int ifindex, in_addr addr, int prefix_len);

/// `ip route replace default via <gw> dev <idx>`
int AddDefaultRoute(int ifindex, in_addr gateway);

/// `ip link add <name> type bridge`; -EEXIST if it already exists.
int CreateBridge(std::string_view name);

/// `ip link add <name> type veth peer name <peer>`.
int CreateVethPair(std::string_view name, std::string_view peer);

/// `ip link del <name>`; -ENODEV if there is no such link.
int DeleteLink(std::string_view name);

/// Netmask from a prefix length (24 -> 255.255.255.0), network order.
in_addr PrefixToNetmask(int prefix_len);

/// Prefix length of a contiguous netmask (255.255.255.0 -> 24), -1 if the
/// mask isn't contiguous.
int NetmaskToPrefix(in_addr netmask);

/// "10.0.3.1/24" -> address and prefix length; false if it isn't an IPv4
/// CIDR (never throws).
bool ParseCidr(std::string_view cidr, in_addr& addr, int& prefix_len);

/// Bridge @p name exists (created if missing), has the address @p cidr
/// (skipped if empty) and is up. 0 or -errno (-EINVAL for a bad CIDR).
int EnsureBridge(std::string_view name, std::string_view cidr);

}  // namespace aember::utils::netlink
