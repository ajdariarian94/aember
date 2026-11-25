/**
 * @file fuzz_dhcp.cpp
 * @brief aember-netd's DHCP client reading a frame off the packet socket:
 *        ParseUdpFrame (IPv4 + UDP, checksums), then Parse (BOOTREPLY and
 *        its options), then the lease timers. The input is tried both as a
 *        whole frame and as a bare DHCP payload.
 *        Properties: never crashes; the UDP payload lies inside the frame;
 *        a lease's renew, rebind and expiry times are in order.
 */
#include <aember-services/netd/dhcp-client.h>

#include <chrono>
#include <span>

#include "common.h"

namespace dhcp = aember::netd::dhcp;

namespace {

void Lease(std::span<const std::uint8_t> payload) {
  const auto message = dhcp::Parse(payload);
  if (!message) return;
  std::ignore = message->lease.PrefixLength();
  const auto t0 = dhcp::LeaseTimer::Clock::now();
  const dhcp::LeaseTimer timer{message->lease, t0};
  if (timer.At(t0) == dhcp::LeaseTimer::Phase::kInfinite) return;
  if (timer.RenewAt() < t0 || timer.RenewAt() > timer.RebindAt() ||
      timer.RebindAt() > timer.ExpiresAt()) {
    fuzz::Fail("lease times out of order");
  }
  // Phases only move forward.
  auto last = timer.At(t0);
  for (const auto s : {1, 60, 3600, 86400, 1 << 30}) {
    const auto phase = timer.At(t0 + std::chrono::seconds{s});
    if (phase < last) fuzz::Fail("lease phase went back");
    last = phase;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  const std::span<const std::uint8_t> frame{data, size};
  const auto payload = dhcp::ParseUdpFrame(frame, 68);
  if (!payload.empty() &&
      (payload.data() < frame.data() ||
       payload.data() + payload.size() > frame.data() + frame.size())) {
    fuzz::Fail("payload outside the frame");
  }
  Lease(payload);
  Lease(frame);
  return 0;
}
