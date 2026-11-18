/**
 * @file dhcp-client.h
 * @author Arian Ajdari
 * @brief Minimal DHCPv4 client (RFC 2131), replacing BusyBox udhcpc.
 *
 *        DISCOVER -> OFFER -> REQUEST -> ACK on a packet socket (AF_PACKET)
 *        bound to the interface, with the IPv4/UDP headers built here, like
 *        udhcpc: the interface has no address yet, and a UDP socket would
 *        send from another interface's address (RFC 2131 wants 0.0.0.0, and
 *        the kernel drops replies that look like they come from a local
 *        address). The broadcast flag is set so servers answer by broadcast.
 *        Like the `udhcpc -q` it replaces, it gets one lease and stops; it
 *        doesn't renew.
 *
 *        Packet building and parsing are pure functions (unit-tested); only
 *        DhcpClient::Acquire touches the network.
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <netinet/in.h>

namespace aember::netd::dhcp {

using Mac = std::array<std::uint8_t, 6>;

enum class MessageType : std::uint8_t {
  kDiscover = 1,
  kOffer = 2,
  kRequest = 3,
  kDecline = 4,
  kAck = 5,
  kNak = 6,
  kRelease = 7,
};

/// What a lease gives us (all addresses in network order).
struct Lease {
  in_addr address{};
  in_addr netmask{};
  std::optional<in_addr> router;
  std::vector<in_addr> dns;
  in_addr server{};
  std::uint32_t lease_seconds{0};
  /// T1/T2 (options 58/59); 0 = not sent (then 50% / 87.5% of the lease).
  std::uint32_t renew_seconds{0};
  std::uint32_t rebind_seconds{0};

  /// Prefix length from the netmask; 24 if the server sent none.
  int PrefixLength() const;
};

/// A parsed server message.
struct Message {
  MessageType type{};
  std::uint32_t xid{0};
  Mac chaddr{};
  Lease lease;
};

/// DHCPDISCOVER for @p mac.
std::vector<std::uint8_t> BuildDiscover(std::uint32_t xid, const Mac& mac);

/// DHCPREQUEST for @p requested from @p server (selecting state).
std::vector<std::uint8_t> BuildRequest(std::uint32_t xid, const Mac& mac,
                                       in_addr requested, in_addr server);

/// DHCPREQUEST renewing/rebinding @p ciaddr (RFC 2131 4.3.2): ciaddr set,
/// no requested-address or server-id option, no broadcast flag.
std::vector<std::uint8_t> BuildRenew(std::uint32_t xid, const Mac& mac,
                                     in_addr ciaddr);

/// DHCPREQUEST in INIT-REBOOT state: "may I keep @p requested?" (after a
/// netd restart found the address already on the interface).
std::vector<std::uint8_t> BuildInitReboot(std::uint32_t xid, const Mac& mac,
                                          in_addr requested);

/// Parse a BOOTREPLY; nullopt if it isn't a valid DHCP reply.
std::optional<Message> Parse(std::span<const std::uint8_t> packet);

/// IPv4 + UDP headers around @p payload (checksums filled in): what goes
/// into an AF_PACKET/ETH_P_IP socket.
std::vector<std::uint8_t> BuildUdpFrame(std::span<const std::uint8_t> payload,
                                        in_addr src, in_addr dst,
                                        std::uint16_t src_port,
                                        std::uint16_t dst_port);

/// The UDP payload of an IPv4 frame if it is UDP to @p dst_port with valid
/// lengths (and a valid checksum when one is set); empty otherwise.
std::span<const std::uint8_t> ParseUdpFrame(std::span<const std::uint8_t> frame,
                                            std::uint16_t dst_port);

/// When a lease must be renewed, rebound, or is gone (RFC 2131 4.4.5).
class LeaseTimer {
 public:
  using Clock = std::chrono::steady_clock;
  enum class Phase {
    kBound,      ///< before T1
    kRenewing,   ///< T1..T2: ask the server (unicast)
    kRebinding,  ///< T2..expiry: ask any server (broadcast)
    kExpired,    ///< the address must go: start over
    kInfinite,   ///< lease without end (0xffffffff)
  };

  LeaseTimer() = default;
  LeaseTimer(const Lease& lease, Clock::time_point acquired);

  [[nodiscard]] Phase At(Clock::time_point now) const;
  [[nodiscard]] Clock::time_point RenewAt() const { return renew_at_; }
  [[nodiscard]] Clock::time_point RebindAt() const { return rebind_at_; }
  [[nodiscard]] Clock::time_point ExpiresAt() const { return expires_at_; }

 private:
  bool infinite_{false};
  Clock::time_point renew_at_{};
  Clock::time_point rebind_at_{};
  Clock::time_point expires_at_{};
};

/// Why a renewal failed: a NAK means the address must go now.
struct RenewError {
  bool nak{false};
  std::string message;
};

class DhcpClient {
 public:
  explicit DhcpClient(std::string interface);

  /// Run DISCOVER/OFFER/REQUEST/ACK; resends DISCOVER every 2 s within
  /// @p timeout. Error text on failure.
  std::expected<Lease, std::string> Acquire(std::chrono::milliseconds timeout);

  /// Renew @p current (unicast to its server) or, with @p broadcast,
  /// rebind it (any server). Uses a UDP socket: the interface has the
  /// address by now. The new lease keeps fields the ACK left out.
  std::expected<Lease, RenewError> Renew(const Lease& current, bool broadcast,
                                         std::chrono::milliseconds timeout);

  /// INIT-REBOOT: may we keep @p address? (no lease known, e.g. after a
  /// netd restart). Broadcast from 0.0.0.0 like Acquire.
  std::expected<Lease, RenewError> Confirm(in_addr address,
                                           std::chrono::milliseconds timeout);

 private:
  std::string interface_;
};

}  // namespace aember::netd::dhcp
