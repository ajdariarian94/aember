/**
 * @file containerd.cpp
 * @author Arian Ajdari
 * @brief `aember containerd`: the containers, out of PID 1.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/netlink/netlink.h>
#include <aember-services/containerd/container-manager.h>
#include <aember-services/containerd/container-state.h>
#include <aember-services/containerd/containerd.h>
#include <aember-services/containerd/control.h>
#include <aember-services/containerd/lxc-api.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace aember::containerd {

namespace {

using aember::containerd::ContainerState;
using aember::containerd::ContainerStateToString;
namespace ipc = aember::utils::ipc;

int PidfdOpen(pid_t pid) {
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
}

}  // namespace

int EnsureBridges(const std::string& containers_json) {
  aember::utils::logging::Logger log{"containerd"};
  std::ifstream file{containers_json};
  const auto config = nlohmann::json::parse(file, nullptr, false);
  if (config.is_discarded() || !config.is_object()) return 0;  // reported later
  const auto it = config.find("bridges");
  if (it == config.end()) return 0;
  if (!it->is_array()) {
    log.error("{}: 'bridges' must be an array", containers_json);
    return 1;
  }
  int failed = 0;
  for (const auto& bridge : *it) {
    const auto name = bridge.value("name", std::string{});
    const auto address = bridge.value("address", std::string{});
    if (name.empty()) {
      log.error("bridge without a name in {}", containers_json);
      ++failed;
      continue;
    }
    if (const int rc = aember::utils::netlink::EnsureBridge(name, address);
        rc != 0) {
      log.error("bridge '{}' ({}): {}",
                name,
                address,
                std::system_category().message(-rc));
      ++failed;
      continue;
    }
    log.info(
        "bridge '{}' up ({})", name, address.empty() ? "no address" : address);
  }
  return failed;
}

int RunContainerd(const ContainerdOptions& options) {
  aember::utils::logging::Logger log{"containerd"};
  ::prctl(PR_SET_NAME, "aember-ctrd");
  // No PR_SET_CHILD_SUBREAPER: the LXC monitors have a parent-death signal,
  // which also fires when a subreaper they were reparented to dies, so a
  // crashed containerd would take every container down. They stay PID 1's
  // orphans (PID 1 never dies); we follow the containers through pidfds,
  // which don't need us to be the parent.

  // Signals through a signalfd; blocked before anything could start a
  // thread.
  sigset_t signals;
  sigemptyset(&signals);
  for (const int s : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) sigaddset(&signals, s);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  // liblxc forks its monitor from us without exec and gives the container
  // init the caller's signal mask: with ours (TERM/INT/HUP/CHLD blocked)
  // nothing in a container could be stopped gracefully, and a shell there
  // never saw its children exit. Children start with nothing blocked.
  pthread_atfork(nullptr, nullptr, [] {
    sigset_t none;
    sigemptyset(&none);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): atfork child handler: one thread
    sigprocmask(SIG_SETMASK, &none, nullptr);  // async-signal-safe
  });
  const int sigfd = ::signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);

  auto channel = ipc::Channel::Inherited();
  if (!channel.Valid()) {
    log.warn("no channel to PID 1 on fd 3; running standalone");
  }
  ipc::ReceiveControl(channel);  // muted if a debug shell is open
  // Keeps what the console doesn't show while a debug shell is open.
  aember::utils::logging::enable_file_logging(options.log_file);

  if (Lxc() == nullptr) {
    log.critical("cannot load liblxc: {}", LxcLoadError());
    return 1;
  }

  ContainerManager containers{
      std::make_shared<aember::mount_manager::MountManager>(),
      [&](const std::string& name, ContainerState, ContainerState state) {
        channel.Send(ipc::Event(
            "container",
            {{"name", name}, {"state", ContainerStateToString(state)}}));
      }};

  // Start (or adopt) every container.
  // Container networking is ours, so it works with any network service
  // (or none). A failed bridge only affects the containers that use it.
  std::ignore = EnsureBridges(options.containers_json);

  const auto configs = containers.LoadContainers(options.containers_json);
  for (const auto& cfg : configs) {
    if (!containers.HasContainer(cfg.name)) containers.AddContainer(cfg);
  }
  for (const auto& cfg : configs) {
    if (!containers.Adopt(cfg.name)) containers.StartContainer(cfg.name);
  }
  channel.Send(ipc::Ready());
  log.info("containerd ready ({} container(s))", configs.size());

  // Watch each running container's init: a pidfd works for processes that
  // aren't our children (the init is the LXC monitor's child).
  std::map<std::string, std::pair<pid_t, int>> watched;  // name -> pid, fd
  const auto watch_running = [&] {
    for (const auto& cfg : configs) {
      if (watched.contains(cfg.name) || !containers.IsRunning(cfg.name)) {
        continue;
      }
      const auto pid = containers.GetInitPid(cfg.name);
      if (!pid) continue;
      const int fd = PidfdOpen(*pid);
      if (fd >= 0) watched[cfg.name] = {*pid, fd};
    }
  };
  watch_running();

  // Restarts (restart_policy): name -> when to start it again.
  using Clock = std::chrono::steady_clock;
  std::map<std::string, Clock::time_point> restart_at;
  const auto schedule_restart = [&](const std::string& name) {
    if (const auto delay = containers.NextRestart(name)) {
      restart_at[name] = Clock::now() + *delay;
    }
  };
  for (const auto& cfg : configs) {
    if (!containers.IsRunning(cfg.name)) schedule_restart(cfg.name);
  }

  // The control socket (the aember-fleet agent): requests act like the
  // restart logic does, a requested stop clears a pending restart.
  struct Control final : ControlTarget {
    ContainerManager& containers;
    const std::vector<ContainerConfig>& configs;
    std::map<std::string, Clock::time_point>& restart_at;
    std::function<void(const std::string&)> schedule_restart;
    Control(ContainerManager& c, const std::vector<ContainerConfig>& cfgs,
            std::map<std::string, Clock::time_point>& r,
            std::function<void(const std::string&)> s)
        : containers(c),
          configs(cfgs),
          restart_at(r),
          schedule_restart(std::move(s)) {}
    [[nodiscard]] std::vector<Entry> List() const override {
      std::vector<Entry> list;
      list.reserve(configs.size());
      for (const auto& cfg : configs) {
        list.push_back(
            {cfg.name,
             ContainerStateToString(containers.GetContainerState(cfg.name))});
      }
      return list;
    }
    [[nodiscard]] bool Has(std::string_view name) const override {
      return containers.HasContainer(name);
    }
    bool Stop(std::string_view name) override {
      restart_at.erase(std::string{name});
      return containers.StopContainer(name);
    }
    bool Start(std::string_view name) override {
      restart_at.erase(std::string{name});
      if (containers.StartContainer(name)) return true;
      schedule_restart(std::string{name});
      return false;
    }
  } control{containers, configs, restart_at, schedule_restart};
  int control_fd = -1;
  if (!options.control_socket.empty()) {
    control_fd = OpenControlSocket(options.control_socket);
    if (control_fd < 0) {
      log.warn("control socket {}: {}",
               options.control_socket,
               std::system_category().message(-control_fd));
    } else {
      log.info("control socket {}", options.control_socket);
    }
  }

  aember::child_supervisor::ChildSupervisor reaper;
  bool stop = false;
  while (!stop) {
    int timeout_ms = 1000;
    for (const auto& [name, at] : restart_at) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            at - Clock::now())
                            .count();
      timeout_ms = std::clamp(static_cast<int>(left), 0, timeout_ms);
    }
    // fds[1]: PID 1's channel (console mute), fds[2]: the control socket;
    // negative = ignored by poll.
    std::vector<pollfd> fds{{sigfd, POLLIN, 0},
                            {channel.Closed() ? -1 : channel.Fd(), POLLIN, 0},
                            {control_fd, POLLIN, 0}};
    std::vector<std::string> names;
    for (const auto& [name, w] : watched) {
      fds.push_back({w.second, POLLIN, 0});
      names.push_back(name);
    }
    if (::poll(fds.data(), fds.size(), timeout_ms) < 0 && errno != EINTR) {
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      signalfd_siginfo si{};
      while (::read(sigfd, &si, sizeof(si)) == sizeof(si)) {
        if (si.ssi_signo != SIGCHLD && si.ssi_signo != SIGHUP) stop = true;
      }
    }

    if (fds[1].revents != 0) ipc::ReceiveControl(channel);
    if ((fds[2].revents & POLLIN) != 0 && !stop) {
      ServeControlConnection(control_fd, control);
    }

    for (std::size_t i = 3; i < fds.size(); ++i) {
      if (fds[i].revents == 0) continue;
      const auto it = watched.find(names[i - 3]);
      log.info(
          "container '{}' init (pid {}) exited", it->first, it->second.first);
      const std::string name = it->first;
      containers.HandleExit(it->second.first, 1);
      ::close(it->second.second);
      watched.erase(it);
      if (!stop) schedule_restart(name);
    }

    // Due restarts (a failed start counts as another attempt).
    for (auto it = restart_at.begin(); !stop && it != restart_at.end();) {
      if (it->second > Clock::now()) {
        ++it;
        continue;
      }
      const std::string name = it->first;
      it = restart_at.erase(it);
      if (!containers.StartContainer(name)) schedule_restart(name);
    }

    // Our own children (helpers liblxc waits for itself are left to it).
    std::ignore = reaper.ReapZombies();
    watch_running();
  }

  restart_at.clear();
  if (control_fd >= 0) {
    ::close(control_fd);
    ::unlink(options.control_socket.c_str());
  }
  log.info("stopping all containers");
  for (const auto& [name, w] : watched) ::close(w.second);
  containers.StopAll();
  log.info("containerd stopped");
  return 0;
}

}  // namespace aember::containerd
