/**
 * @file netd.cpp
 * @author Arian Ajdari
 * @brief `aember netd`: the network, out of PID 1.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-services/netd/netd.h>
#include <aember-services/netd/network-manager.h>

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <memory>
#include <mutex>

#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace aember::netd {

namespace {

namespace ipc = aember::utils::ipc;
constexpr int kReportEveryMs = 10'000;

}  // namespace

int RunNetd(const NetdOptions& options) {
  aember::utils::logging::Logger log{"netd"};
  ::prctl(PR_SET_NAME, "aember-netd");

  sigset_t signals;
  sigemptyset(&signals);
  for (const int s : {SIGTERM, SIGINT, SIGHUP}) sigaddset(&signals, s);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);  // before any thread
  const int sigfd = ::signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);

  auto channel = ipc::Channel::Inherited();
  if (!channel.Valid()) {
    log.warn("no channel to PID 1 on fd 3; running standalone");
  }
  ipc::ReceiveControl(channel);  // muted if a debug shell is open
  // Keeps what the console doesn't show while a debug shell is open.
  aember::utils::logging::enable_file_logging("/var/log/aember-netd.log");

  nlohmann::json config;
  {
    std::ifstream file{options.network_json};
    config = nlohmann::json::parse(file, nullptr, false);
  }
  if (config.is_discarded() || !config.is_object()) {
    log.critical("cannot read {}", options.network_json);
    return 1;
  }

  std::mutex send_mutex;  // the status callback runs on the monitor thread
  std::unique_ptr<NetworkManager> network;
  const auto report = [&](const NetworkManager::ConnectivityStatus& s) {
    nlohmann::json data{
        {"online", s.online}, {"interface", s.interface}, {"rtt_ms", s.rtt_ms}};
    if (network) data["ip"] = network->GetNetworkInfo().ip_address;
    std::lock_guard lock{send_mutex};
    channel.Send(ipc::Event("network", std::move(data)));
  };

  try {
    network = std::make_unique<NetworkManager>(config, report);
    network->Start();  // interfaces, DHCP, then the monitor thread
  } catch (const std::exception& e) {
    // PID 1 restarts us; a required interface that never comes up keeps
    // failing here, which is what it should do.
    log.critical("network bring-up failed: {}", e.what());
    return 1;
  }

  {
    std::lock_guard lock{send_mutex};
    channel.Send(ipc::Ready());
  }
  log.info("netd ready");

  while (true) {
    // A negative fd is ignored by poll: no channel, or PID 1 closed it.
    std::array<pollfd, 2> fds{
        {{sigfd, POLLIN, 0},
         {channel.Closed() ? -1 : channel.Fd(), POLLIN, 0}}};
    const int r = ::poll(fds.data(), fds.size(), kReportEveryMs);
    if (r > 0) {
      if (fds[1].revents != 0) ipc::ReceiveControl(channel);
      if ((fds[0].revents & POLLIN) == 0) continue;
      signalfd_siginfo si{};
      if (::read(sigfd, &si, sizeof(si)) == sizeof(si) &&
          si.ssi_signo != SIGHUP) {
        break;
      }
      continue;
    }
    report(network->GetStatus());
  }

  log.info("netd stopping (network configuration stays)");
  network->Stop();
  return 0;
}

}  // namespace aember::netd
