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
#include <aember-libs/utils/package/install.h>
#include <aember-services/containerd/container-manager.h>
#include <aember-services/containerd/container-parser.h>
#include <aember-services/containerd/container-state.h>
#include <aember-services/containerd/containerd.h>
#include <aember-services/containerd/control.h>
#include <aember-services/containerd/lxc-api.h>
#include <aember-services/containerd/start-graph.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
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
  std::vector<std::string> errors;
  const auto bridges = ParseBridges(config, errors);
  for (const auto& error : errors) log.error("{}: {}", containers_json, error);
  int failed = static_cast<int>(errors.size());
  for (const auto& [name, address] : bridges) {
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

utils::package::InstallOptions MakeInstallOptions(
    const ContainerdOptions& options, bool allow_unsigned) {
  namespace pkg = aember::utils::package;
  aember::utils::logging::Logger log{"containerd"};
  std::vector<std::string> errors;
  pkg::InstallOptions install{
      .root = options.packages_root,
      .policy = {.keys = pkg::LoadKeys(options.keys_dir, errors),
                 .allow_unsigned = allow_unsigned},
      .check_config = [root =
                           options.packages_root](const nlohmann::json& config)
          -> std::expected<void, std::string> {
        auto parsed = ParseContainerConfig(config, root);
        if (!parsed) return std::unexpected(parsed.error());
        return {};
      }};
  for (const auto& error : errors) log.error("key {}", error);
  return install;
}

int InstallPackages(const ContainerdOptions& options, bool allow_unsigned) {
  namespace pkg = aember::utils::package;
  aember::utils::logging::Logger log{"containerd"};

  std::vector<std::string> notes;
  pkg::Recover(options.packages_root, notes);
  for (const auto& note : notes) log.warn("recovered an install: {}", note);

  std::error_code ec;
  if (!std::filesystem::is_directory(options.inbox, ec)) return 0;
  std::vector<std::string> packages;
  for (const auto& entry :
       std::filesystem::directory_iterator{options.inbox, ec}) {
    if (entry.path().extension() == pkg::kExtension) {
      packages.push_back(entry.path().string());
    }
  }
  if (packages.empty()) return 0;
  std::ranges::sort(packages);

  const auto install = MakeInstallOptions(options, allow_unsigned);

  int failed = 0;
  for (const auto& package : packages) {
    const auto result = pkg::Install(package, install);
    if (!result) {
      log.error("not installed: {}", result.error());
      std::filesystem::rename(package, package + ".rejected", ec);
      ++failed;
      continue;
    }
    log.info("{} {} ({}, {})",
             result->changed ? "installed" : "already installed",
             result->name,
             result->signed_by ? "signed by " + *result->signed_by
                               : std::string{"unsigned"},
             result->digest.substr(0, 16));
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

  // The state events come from the threads that start containers too: one
  // datagram each, sent whole.
  std::mutex send_mutex;
  const auto report = [&](const std::string& name, std::string_view state) {
    std::lock_guard lock{send_mutex};
    channel.Send(ipc::Event("container",
                            {{"name", name}, {"state", std::string{state}}}));
  };
  ContainerManager containers{
      std::make_shared<aember::mount_manager::MountManager>(),
      [&](const std::string& name, ContainerState, ContainerState state) {
        report(name, ContainerStateToString(state));
      }};

  // Container networking is ours, so it works with any network service
  // (or none). A failed bridge only affects the containers that use it.
  std::ignore = EnsureBridges(options.containers_json);

  // The board's list; a broken one starts nothing (logged).
  BoardContainers board;
  std::error_code ec;
  if (std::filesystem::exists(options.containers_json, ec)) {
    auto loaded = LoadBoardContainers(options.containers_json);
    if (loaded) {
      board = std::move(*loaded);
    } else {
      log.error("{}", loaded.error());
    }
  }
  // New packages (the image's first boot, an update) before anything runs.
  std::ignore = InstallPackages(options, board.allow_unsigned);

  StartGraph graph{board.containers};
  for (const auto& [name, why] : graph.Skipped()) {
    log.error("container '{}' not started: {}", name, why);
  }
  std::vector<std::string> names;  // registered, in file order
  for (const auto& name : graph.Names()) {
    auto cfg = LoadContainerConfig(name, options.packages_root);
    if (!cfg) {
      log.error("container '{}': {}", name, cfg.error());
      report(name, "failed");
      continue;
    }
    containers.AddContainer(*cfg);
    names.push_back(name);
    if (containers.Adopt(name)) {
      graph.SetRunning(name, true);
    } else {
      graph.Want(name);
    }
  }
  log.info("{} container(s), {} start wave(s)", names.size(), graph.Depth());

  // Starts run in threads (independent containers start together); a
  // finished one is reported through `done` and the eventfd.
  const int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  std::mutex done_mutex;
  std::vector<std::pair<std::string, bool>> done;
  std::map<std::string, std::jthread> jobs;
  bool stop = false;
  const auto launch_ready = [&] {
    if (stop) return;
    for (const auto& name : graph.TakeReady()) {
      jobs[name] = std::jthread{[&, name] {
        const bool ok = containers.StartContainer(name);
        {
          std::lock_guard lock{done_mutex};
          done.emplace_back(name, ok);
        }
        const std::uint64_t one = 1;
        std::ignore = ::write(wake, &one, sizeof(one));
      }};
    }
  };

  // Restarts (restart_policy): name -> when to want it again.
  using Clock = std::chrono::steady_clock;
  std::map<std::string, Clock::time_point> restart_at;
  const auto schedule_restart = [&](const std::string& name) {
    if (const auto delay = containers.NextRestart(name)) {
      restart_at[name] = Clock::now() + *delay;
    }
  };
  const auto finish_jobs = [&] {
    std::vector<std::pair<std::string, bool>> finished;
    {
      std::lock_guard lock{done_mutex};
      finished.swap(done);
    }
    for (const auto& [name, ok] : finished) {
      jobs.erase(name);  // joins (the thread has ended or is ending)
      graph.SetRunning(name, ok);
      if (!ok && !stop) schedule_restart(name);
    }
  };

  // The control socket (the aember-fleet agent): requests act like the
  // restart logic does, a requested stop clears a pending restart.
  struct Control final : ControlTarget {
    std::function<std::vector<Entry>()> list;
    std::function<bool(std::string_view)> has, stop, start;
    std::function<std::expected<void, std::string>(std::string_view, bool)>
        replace;
    [[nodiscard]] std::vector<Entry> List() const override { return list(); }
    [[nodiscard]] bool Has(std::string_view n) const override { return has(n); }
    bool Stop(std::string_view n) override { return stop(n); }
    bool Start(std::string_view n) override { return start(n); }
    std::expected<void, std::string> Install(std::string_view n) override {
      return replace(n, true);
    }
    std::expected<void, std::string> Rollback(std::string_view n) override {
      return replace(n, false);
    }
  } control;
  control.list = [&] {
    std::vector<ControlTarget::Entry> list(names.size());
    std::ranges::transform(names, list.begin(), [&](const std::string& name) {
      return ControlTarget::Entry{
          name, ContainerStateToString(containers.GetContainerState(name))};
    });
    return list;
  };
  control.has = [&](std::string_view n) { return containers.HasContainer(n); };
  control.stop = [&](std::string_view n) {
    const std::string name{n};
    if (graph.IsStarting(name)) return false;  // busy: ask again
    restart_at.erase(name);
    graph.Unwant(name);
    const bool ok = containers.StopContainer(name);
    graph.SetRunning(name, false);
    return ok;
  };
  control.start = [&](std::string_view n) {
    const std::string name{n};
    restart_at.erase(name);
    if (graph.IsStarting(name) || graph.IsRunning(name)) return true;
    if (!graph.DependenciesRun(name)) {
      graph.Want(name);  // starts as soon as they do
      return true;
    }
    const bool ok = containers.StartContainer(name);
    graph.SetRunning(name, ok);
    if (!ok) schedule_restart(name);
    return ok;
  };
  // An update (install) or a rollback: the container stops, its directory
  // changes, it starts again with the config that is there now.
  control.replace = [&](std::string_view n,
                        bool install) -> std::expected<void, std::string> {
    namespace pkg = aember::utils::package;
    const std::string name{n};
    if (graph.IsStarting(name)) return std::unexpected("it is starting");
    restart_at.erase(name);
    graph.Unwant(name);
    containers.StopContainer(name);
    graph.SetRunning(name, false);
    std::expected<void, std::string> result;
    if (install) {
      const auto opts = MakeInstallOptions(options, board.allow_unsigned);
      const auto r = pkg::Install(
          options.inbox + "/" + name + std::string{pkg::kExtension}, opts);
      if (r) {
        log.info("installed {} ({}, {})",
                 name,
                 r->signed_by ? "signed by " + *r->signed_by
                              : std::string{"unsigned"},
                 r->digest.substr(0, 16));
      } else {
        result = std::unexpected(r.error());
      }
    } else {
      result = pkg::Rollback(options.packages_root, name);
      if (result) log.info("rolled {} back", name);
    }
    // What is in place now (the new version, or the old one if the install
    // failed), started again either way.
    if (auto cfg = LoadContainerConfig(name, options.packages_root); cfg) {
      containers.UpdateConfig(*cfg);
    } else if (result) {
      result = std::unexpected(cfg.error());
    }
    control.start(name);
    return result;
  };
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

  // Watch each running container's init: a pidfd works for processes that
  // aren't our children (the init is the LXC monitor's child).
  std::map<std::string, std::pair<pid_t, int>> watched;  // name -> pid, fd
  const auto watch_running = [&] {
    for (const auto& name : names) {
      if (watched.contains(name) || !containers.IsRunning(name)) continue;
      const auto pid = containers.GetInitPid(name);
      if (!pid) continue;
      const int fd = PidfdOpen(*pid);
      if (fd >= 0) watched[name] = {*pid, fd};
    }
  };

  launch_ready();
  bool ready_sent = false;
  aember::child_supervisor::ChildSupervisor reaper;
  while (!stop) {
    finish_jobs();
    launch_ready();
    watch_running();
    // Ready once the first wave is through: nothing starting, nothing more
    // that can start now (an entry waiting for a failed one keeps waiting).
    if (!ready_sent && !graph.Busy()) {
      channel.Send(ipc::Ready());
      ready_sent = true;
      log.info("containerd ready ({} container(s))", names.size());
    }

    int timeout_ms = 1000;
    for (const auto& [name, at] : restart_at) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            at - Clock::now())
                            .count();
      timeout_ms = std::clamp(static_cast<int>(left), 0, timeout_ms);
    }
    // fds[1]: PID 1's channel (console mute), fds[2]: the control socket,
    // fds[3]: a start finished; negative = ignored by poll.
    std::vector<pollfd> fds{{sigfd, POLLIN, 0},
                            {channel.Closed() ? -1 : channel.Fd(), POLLIN, 0},
                            {control_fd, POLLIN, 0},
                            {wake, POLLIN, 0}};
    std::vector<std::string> watched_names;
    for (const auto& [name, w] : watched) {
      fds.push_back({w.second, POLLIN, 0});
      watched_names.push_back(name);
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
    if ((fds[3].revents & POLLIN) != 0) {
      std::uint64_t count = 0;
      std::ignore = ::read(wake, &count, sizeof(count));
    }
    finish_jobs();
    if ((fds[2].revents & POLLIN) != 0 && !stop) {
      ServeControlConnection(control_fd, control);
    }

    for (std::size_t i = 4; i < fds.size(); ++i) {
      if (fds[i].revents == 0) continue;
      const auto it = watched.find(watched_names[i - 4]);
      log.info(
          "container '{}' init (pid {}) exited", it->first, it->second.first);
      const std::string name = it->first;
      containers.HandleExit(it->second.first, 1);
      ::close(it->second.second);
      watched.erase(it);
      graph.SetRunning(name, false);
      if (!stop) schedule_restart(name);
    }

    // Due restarts: wanted again, they start once their dependencies run.
    for (auto it = restart_at.begin(); !stop && it != restart_at.end();) {
      if (it->second > Clock::now()) {
        ++it;
        continue;
      }
      graph.Want(it->first);
      it = restart_at.erase(it);
    }

    // Our own children (helpers liblxc waits for itself are left to it).
    std::ignore = reaper.ReapZombies();
  }

  // No new starts (spec/tla/StartGraph.tla, NoStartAfterShutdown); the ones
  // in progress finish first, then everything stops, dependents first.
  restart_at.clear();
  jobs.clear();  // joins
  finish_jobs();
  if (control_fd >= 0) {
    ::close(control_fd);
    ::unlink(options.control_socket.c_str());
  }
  log.info("stopping all containers");
  for (const auto& [name, w] : watched) ::close(w.second);
  for (const auto& name : graph.StopOrder()) {
    if (containers.HasContainer(name)) containers.StopContainer(name);
  }
  containers.StopAll();  // anything left (adopted, not in the graph)
  if (wake >= 0) ::close(wake);
  log.info("containerd stopped");
  return 0;
}

}  // namespace aember::containerd
