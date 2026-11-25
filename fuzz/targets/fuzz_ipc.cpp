/**
 * @file fuzz_ipc.cpp
 * @brief The daemon channel (fd 3): any bytes a daemon could send arrive
 *        through a real SOCK_SEQPACKET pair at Channel::Receive, then go to
 *        what PID 1 does with them (ApplyDaemonMessage) and what a daemon
 *        does with PID 1's messages (ApplyControl).
 *        Properties: never crashes or throws; a received message is a JSON
 *        object; Send of what was received (and of a message holding the
 *        raw bytes as a string, valid UTF-8 or not) never throws and
 *        arrives as JSON.
 */
#include <aember-libs/aember-init/daemon-status.h>
#include <aember-libs/utils/ipc/channel.h>

#include <chrono>
#include <utility>

#include <sys/socket.h>

#include "common.h"

namespace ipc = aember::utils::ipc;

namespace {

std::pair<ipc::Channel, ipc::Channel>& Pair() {
  static auto pair = [] {
    auto p = ipc::Channel::Pair();
    if (!p) fuzz::Fail("no channel pair");
    return std::move(*p);
  }();
  return pair;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  auto& [daemon, pid1] = Pair();
  static aember::aember_init::DaemonStatus status;

  if (size > 0 && size <= ipc::kMaxMessage) {
    if (::send(daemon.Fd(), data, size, MSG_NOSIGNAL) !=
        static_cast<ssize_t>(size)) {
      fuzz::Fail("send");
    }
    const auto message = pid1.Receive(std::chrono::milliseconds{1000});
    if (message) {
      if (!message->is_object()) fuzz::Fail("received a non-object");
      std::ignore = aember::aember_init::ApplyDaemonMessage(*message, status);
      std::ignore = ipc::ApplyControl(*message);
      if (!pid1.Send(*message)) fuzz::Fail("echo of a received message");
      if (!daemon.Receive(std::chrono::milliseconds{1000})) {
        fuzz::Fail("echo didn't arrive");
      }
    }
  }

  // A daemon reporting raw bytes (an interface name, say) as a string.
  const auto event = ipc::Event(
      "network", {{"interface", std::string{fuzz::Text(data, size)}}});
  if (pid1.Send(event, /*wait=*/false)) {
    const auto back = daemon.Receive(std::chrono::milliseconds{1000});
    if (!back || ipc::TypeOf(*back) != "event") fuzz::Fail("event lost");
  }
  aember::utils::logging::set_console_logging(false);  // ApplyControl
  return 0;
}
