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

#include <string_view>

#include <netinet/in.h>

namespace aember::network::netlink {

/// Interface index, or -1 if there is no such interface.
int InterfaceIndex(std::string_view name);

/// `ip link set <idx> up`
int SetLinkUp(int ifindex);

/// `ip addr replace <addr>/<prefix> dev <idx>` (also sets the broadcast).
int AddAddress(int ifindex, in_addr addr, int prefix_len);

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

}  // namespace aember::network::netlink
