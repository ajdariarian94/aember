/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of the AemberInit init system.
 */

#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/device-health/status.h>
#include <aember-libs/utils/netlink/netlink.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <sys/wait.h>

namespace aember::aember_init {

namespace {

/// Run one init phase. PID 1 must never die, so an exception from a phase is
/// logged and init carries on with the next one.
template <typename F>
bool Guarded(aember::utils::logging::Logger& log, std::string_view phase,
             F&& fn) {
  try {
    std::forward<F>(fn)();
    return true;
  } catch (const std::exception& e) {
    log.error("{} failed: {}", phase, e.what());
  } catch (...) { log.error("{} failed: unknown exception", phase); }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

AemberInit::AemberInit(std::string_view logger_name, Options options)
    : options_(std::move(options)), log_(std::string{logger_name}) {
#ifndef NDEBUG
  aember::utils::logging::logging_level_debug();
#else
  aember::utils::logging::logging_level_info();
#endif
}

AemberInit::~AemberInit() {
  Stop();
}

// ---------------------------------------------------------------------------
// StartInitramfs
// ---------------------------------------------------------------------------

void AemberInit::StartInitramfs() {
  if (running_.exchange(true)) return;

  log_.info("Starting Aember in initramfs mode");

  Guarded(log_, "Initramfs mounts", [this] {
    mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();
    mount_manager_->MountInitramfsFilesystems();
    if (!mount_manager_->MountEarlyFilesystems()) {
      log_.warn("Some early filesystems failed to mount, continuing anyway");
    }
  });

  log_.info("Initramfs detected, preparing to pivot to real root");

  // PerformPivot exec's switch_root on success and does not return.
  Guarded(log_, "Pivot to real root", [this] {
    root_manager_ =
        std::make_unique<aember::root_manager::RootManager>(mount_manager_);

    aember::utils::root::RootConfig root_config;
    root_config.ParseFromProcCmdline();

    if (!mount_manager_->EnsureDirectory(root_config.new_root_path)) {
      log_.error("Failed to create pivot mount point: {}",
                 root_config.new_root_path);
    }

    if (!root_manager_->PerformPivot(root_config)) {
      log_.warn("Pivot to real root failed");
    }
  });

  log_.warn("Continuing in initramfs");

  Guarded(log_, "Signal init", [this] { std::ignore = InitSignals(); });
  StartReaperThread();

  // Emergency shell: the real root is unusable, so open one right away.
  StartDebugShell(/*shell_now=*/true);

  Guarded(log_, "Heartbeat", [this] {
    heartbeat_.emplace([this](const nlohmann::json& p) { OnHeartbeat(p); });
    heartbeat_->Start();
  });

  log_.info("Aember Init System started successfully");
  RunLoop();
  Stop();
}

// ---------------------------------------------------------------------------
// StartRoot
// ---------------------------------------------------------------------------

void AemberInit::BringUpLoopback() {
  namespace netlink = aember::utils::netlink;
  const int index = netlink::InterfaceIndex("lo");
  if (index < 0) {
    log_.warn("No loopback interface");
    return;
  }
  if (const int rc = netlink::SetLinkUp(index); rc < 0) {
    log_.warn("Cannot bring up lo: {}", std::system_category().message(-rc));
    return;
  }
  log_.info("Loopback up");
}

void AemberInit::StartRoot() {
  if (running_.exchange(true)) return;

  aember::utils::logging::enable_file_logging(options_.log_dir +
                                              "/aember-init.log");
  log_.info("Starting Aember in root mode{}",
            options_.chroot ? " (chroot)" : "");

  // The status file exists from the start, so a container that reads it
  // (aember-stats, through /host-logs) never finds it missing.
  {
    std::ofstream file{options_.log_dir + "/aember-status.json"};
    file << "{}";
  }

  Guarded(log_, "Mount phase", [this] {
    std::ignore = InitMounts().or_else([this](const Error& e) -> Result<> {
      log_.warn("Mount phase failed: {}", e.message);
      return {};
    });
  });

  // Loopback is PID 1's job, not the network service's: every board has it,
  // whatever manages the network (aember-netd, ConnMan or nothing), and
  // local clients (SSH to 127.0.0.1, D-Bus over TCP, ...) hang while it is
  // down. Idempotent, so a chroot (already up) is fine.
  Guarded(log_, "Loopback", [this] { BringUpLoopback(); });

  // Signals and the reaper come before any service starts, so exits during
  // StartAll are collected and a shutdown request works from here on.
  Guarded(log_, "Signal init", [this] { std::ignore = InitSignals(); });

  const bool stack_ok = Guarded(log_, "Service stack init", [this] {
    if (auto r = InitServiceStack(); !r) {
      throw std::runtime_error(r.error().message);
    }
  });
  if (!stack_ok) { service_manager_.reset(); }

  StartReaperThread();

  // Drivers are loaded by now (mount phase). Never on a chroot: that would
  // be the host's watchdog.
  if (!options_.chroot) {
    Guarded(log_, "Watchdog", [this] { StartWatchdog(); });
  }

  // From here on 'd' on the console opens a shell at any time; boot does not
  // wait for it. aember.shell=1 on the kernel command line opens one now.
  StartDebugShell(DebugShell::RequestedOnCmdline());

  if (service_manager_) {
    Guarded(log_, "Loading services", [this] {
      std::ignore =
          LoadAndRegisterProcesses(options_.config_dir + "/services.json")
              .or_else([this](const Error& e) -> Result<> {
                log_.warn("{}", e.message);
                return {};
              });
    });

    Guarded(
        log_, "Starting services", [this] { service_manager_->StartAll(); });
  } else {
    log_.error("No service manager: no services");
  }

  Guarded(log_, "Status and heartbeat", [this] {
    // StatusReporter reads PID1 stats from /proc.
    status_ = std::make_unique<aember::device_health::StatusReporter>();
    heartbeat_.emplace([this](const nlohmann::json& p) { OnHeartbeat(p); });
    heartbeat_->Start();
  });

  log_.info("Aember Init System started successfully");

  RunLoop();
  Stop();
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void AemberInit::Stop() {
  if (!running_.exchange(false)) return;

  log_.info("Stopping Aember Init System...");

  // The run loop has ended and the reaper stops below: keep the watchdog
  // fed while services stop (containerd may take a minute).
  if (watchdog_) { watchdog_->SetUnconditional(true); }

  // Gives the console back; a running shell is killed with everything else.
  if (debug_shell_) { debug_shell_->Stop(); }

  // Services stop while the reaper still runs: it reports their exits.
  Guarded(log_, "Stopping services", [this] {
    if (service_manager_) { service_manager_->StopAll(); }
  });

  // Join before the managers it calls into are destroyed below.
  StopReaperThread();
  hub_.Stop();

  signal_handler_.Stop();
  child_supervisor_.StopAll();
  cv_.notify_all();

  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }
  status_.reset();

  // Destroy in reverse construction order.
  service_manager_.reset();
  dependency_resolver_.reset();
  process_manager_.reset();
  mount_manager_.reset();

  // A requested reboot/poweroff/halt: turn the watchdog off so it can't
  // reset the board halfway (pid1::Finish does the rest). Without a request
  // PID 1 is about to reap forever; then the watchdog stays armed and
  // resets the board, which is what a broken init should get.
  if (watchdog_) {
    if (shutdown_.load() != ShutdownAction::kNone) {
      if (!watchdog_->Disarm()) {
        log_.warn("Watchdog can't be stopped; the reset may come first");
      }
    } else {
      log_.critical("Stopping without a shutdown request: watchdog left armed");
    }
    watchdog_.reset();
  }

  log_.info("Aember Init System stopped");
}

// ---------------------------------------------------------------------------
// Init phases
// ---------------------------------------------------------------------------

AemberInit::Result<> AemberInit::InitMounts() {
  mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();

  if (!mount_manager_->MountEarlyFilesystems()) {
    return std::unexpected(Error{"Some early filesystems failed to mount"});
  }

  if (options_.chroot) {
    log_.info("chroot mode: not loading kernel modules (host kernel)");
    return {};
  }

  module_loader_.emplace();
  auto modules =
      module_loader_->LoadModules(options_.config_dir + "/modules.json");
  if (!module_loader_->Load(modules)) {
    log_.warn("Some kernel modules failed to load, continuing anyway");
  }

  return {};
}

AemberInit::Result<> AemberInit::InitServiceStack() {
  process_manager_ =
      std::make_unique<aember::process_manager::ProcessManager>();

  dependency_resolver_ =
      std::make_unique<aember::service_manager::DependencyResolver>(
          [this](const std::string& name) {
            return service_manager_ &&
                   service_manager_->GetState(name) == ProcessState::Running;
          });

  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      *process_manager_, *dependency_resolver_, child_supervisor_);

  // Wire restart callback — ProcessManager calls this after restart delay
  // which triggers ServiceManager to re-launch the process.
  process_manager_->SetRestartCallback([this](const std::string& name) {
    log_.info("Restarting process '{}'", name);
    service_manager_->Start(name);
  });

  service_manager_->SetStateChangeCallback([this](const std::string& name,
                                                  ProcessState old_state,
                                                  ProcessState new_state) {
    OnServiceStateChange(name, old_state, new_state);
  });

  // Daemons started with `ipc` hand their channel to the hub, which turns
  // their messages into readiness and status.
  process_manager_->SetChannelCallback(
      [this](const std::string& name, aember::utils::ipc::Channel channel) {
        hub_.Add(name, std::move(channel));
      });
  hub_.Start([this](const std::string& name, const nlohmann::json& message) {
    OnDaemonMessage(name, message);
  });

  return {};
}

void AemberInit::OnDaemonMessage(const std::string& name,
                                 const nlohmann::json& message) {
  DaemonUpdate update;
  {
    std::lock_guard lock{status_mutex_};
    update = ApplyDaemonMessage(message, daemon_status_);
  }
  switch (update.kind) {
    case DaemonUpdate::Kind::kReady:
      if (service_manager_) { service_manager_->MarkReady(name); }
      break;
    case DaemonUpdate::Kind::kNetwork:
      if (update.went_offline) {
        log_.warn("Network: internet connectivity lost");
      }
      break;
    case DaemonUpdate::Kind::kContainer:
      log_.info("Container '{}': {}", update.container, update.state);
      break;
    case DaemonUpdate::Kind::kIgnored:
      break;
  }
}

AemberInit::Result<> AemberInit::InitSignals() {
  // Handlers run on the signal thread: they only record the request and
  // wake the main thread, which does the actual Stop(). Calling Stop() here
  // would join the signal thread from itself (std::system_error, terminate).
  // Same signals as BusyBox init, so its reboot/poweroff/halt work.
  signal_handler_.Register(
      SIGTERM, [this](int) { RequestShutdown(ShutdownAction::kReboot); });
  signal_handler_.Register(
      SIGINT, [this](int) { RequestShutdown(ShutdownAction::kReboot); });
  signal_handler_.Register(
      SIGUSR2, [this](int) { RequestShutdown(ShutdownAction::kPowerOff); });
  signal_handler_.Register(
      SIGUSR1, [this](int) { RequestShutdown(ShutdownAction::kHalt); });
  signal_handler_.Register(SIGHUP, [this](int) { /* reload config */ });

  signal_handler_.Register(SIGCHLD, [this](int) {
    {
      std::lock_guard lock{reap_mtx_};
      reap_pending_ = true;
    }
    reap_cv_.notify_one();
  });

  signal_handler_.Start();
  return {};
}

AemberInit::Result<> AemberInit::LoadAndRegisterProcesses(
    std::string_view path) {
  auto configs =
      process_manager_->LoadProcesses(std::string{path}, options_.services_dir);
  log_.info("Loaded {} process config(s) from {}", configs.size(), path);

  // A plain loop: AddProcess has a side effect, so it must run exactly once
  // per entry. (It used to sit in a views::filter predicate, which
  // ranges::to may evaluate twice; the second call failed as a duplicate
  // and dropped every entry but the first.)
  for (const auto& cfg : configs) {
    if (!process_manager_->AddProcess(cfg)) {
      log_.error("Failed to register process '{}'", cfg.name);
      continue;
    }
    if (!service_manager_->AddProcess(cfg.name)) {
      log_.error("Failed to add process '{}' to service manager", cfg.name);
      continue;
    }
    log_.info("Registered process '{}'", cfg.name);
  }

  return {};
}

void AemberInit::StartReaperThread() {
  if (reaper_thread_.joinable()) return;

  reaper_thread_ = std::jthread{[this](std::stop_token st) {
    while (!st.stop_requested()) {
      {
        std::unique_lock lock{reap_mtx_};
        reap_cv_.wait_for(lock, st, std::chrono::seconds(1), [this] {
          return reap_pending_;
        });
        reap_pending_ = false;
      }
      reap_tick_ms_ = NowMs();
      Guarded(log_, "Reaping", [this] {
        ReapOnce();
        if (service_manager_) { service_manager_->CheckReadyTimeouts(); }
      });
    }
  }};
}

void AemberInit::StartDebugShell(bool shell_now) {
  Guarded(log_, "Debug shell", [this, shell_now] {
    debug_shell_.emplace();
    // The daemons log to the same console: mute them too while a shell is
    // open (also the ones started or restarted meanwhile).
    debug_shell_->SetConsoleCallback([this](bool shell_open) {
      hub_.Publish("console", aember::utils::ipc::Console(!shell_open));
    });
    // The reaper collects the shell: track it, and it is alive until reaped.
    debug_shell_->Start(
        [this](pid_t pid) { child_supervisor_.AddChild(pid, "debug-shell"); },
        [this](pid_t pid) { return child_supervisor_.IsTracked(pid); },
        shell_now,
        options_.console);
  });
}

std::int64_t AemberInit::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool AemberInit::Alive() const {
  // Both tick at least once a second; 10 s without a tick means stuck.
  // Until the run loop starts (services still starting), boot gets 2 min.
  constexpr std::int64_t kStaleMs = 10'000;
  constexpr std::int64_t kBootMs = 120'000;
  const auto now = NowMs();
  const bool loop_ok = loop_started_.load()
                           ? now - loop_tick_ms_.load() < kStaleMs
                           : now - watchdog_armed_ms_.load() < kBootMs;
  return loop_ok && now - reap_tick_ms_.load() < kStaleMs;
}

void AemberInit::StartWatchdog() {
  const auto options = aember::utils::watchdog::OptionsFromCmdline();
  if (!options) {
    log_.warn("Watchdog disabled (aember.watchdog=0)");
    return;
  }
  watchdog_ = aember::utils::watchdog::Watchdog::Open(*options);
  if (!watchdog_) return;
  watchdog_armed_ms_ = NowMs();
  reap_tick_ms_ = NowMs();
  watchdog_->Start([this] { return Alive(); });
}

void AemberInit::StopReaperThread() {
  if (!reaper_thread_.joinable()) return;
  reaper_thread_.request_stop();
  reaper_thread_.join();
}

void AemberInit::ReapOnce() {
  for (const auto& exit : child_supervisor_.ReapZombies()) {
    if (exit.tracked) {
      log_.debug(
          "Reaped '{}' (pid={} status={})", exit.name, exit.pid, exit.status);
    } else {
      log_.debug("Reaped orphan pid={} status={}", exit.pid, exit.status);
    }
    if (service_manager_) {
      service_manager_->HandleExit(exit.pid, exit.status);
    }
  }
}

void AemberInit::RequestShutdown(ShutdownAction action) {
  auto expected = ShutdownAction::kNone;
  if (!shutdown_.compare_exchange_strong(expected, action)) return;

  log_.info("Shutdown requested: {}", pid1::ToString(action));
  { std::lock_guard lock{mtx_}; }
  cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void AemberInit::RunLoop() {
  log_.info("Aember Init run loop started");
  std::unique_lock lock{mtx_};
  loop_started_ = true;
  while (running_.load() && shutdown_.load() == ShutdownAction::kNone) {
    loop_tick_ms_ = NowMs();
    cv_.wait_for(lock, std::chrono::seconds(1));
  }
  log_.info("Aember Init run loop exiting");
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void AemberInit::OnHeartbeat(const nlohmann::json& base) {
  if (!status_) return;

  // Collect PID1 stats via /proc.
  auto payload = status_->Snapshot();

  // Network status and container states as the daemons last reported them.
  nlohmann::json containers = nlohmann::json::array();
  std::map<std::string, long long> since;
  {
    std::lock_guard lock{status_mutex_};
    if (!daemon_status_.network.is_null()) {
      payload["network"] = daemon_status_.network;
    }
    for (const auto& [name, state] : daemon_status_.containers) {
      containers.push_back(
          {{"name", name}, {"state", state}, {"type", "container"}});
      if (const auto it = daemon_status_.since_ms.find(name);
          it != daemon_status_.since_ms.end()) {
        containers.back()["since_ms"] = it->second;
      }
    }
    since = daemon_status_.since_ms;
  }

  // Enrich with service states.
  if (service_manager_) {
    auto services = nlohmann::json::array();
    for (const auto& name : service_manager_->GetNames()) {
      const auto state = service_manager_->GetState(name);
      services.push_back({
          {"name", name},
          {"state", aember::utils::process::ToString(state)},
          {"type", "process"},
      });
      if (const auto it = since.find(name); it != since.end()) {
        services.back()["since_ms"] = it->second;
      }
    }
    for (auto& c : containers) { services.push_back(std::move(c)); }
    payload["services"] = services;
  }

  // Merge heartbeat base fields (timestamp, status).
  payload.merge_patch(base);

  status_->WriteFile(payload, options_.log_dir + "/aember-status.json");
}

void AemberInit::OnServiceStateChange(const std::string& name,
                                      ProcessState old_state,
                                      ProcessState new_state) {
  {
    std::lock_guard lock{status_mutex_};
    daemon_status_.since_ms[name] = BootTimeMs();
  }
  log_.info("Service '{}': {} -> {}",
            name,
            aember::utils::process::ToString(old_state),
            aember::utils::process::ToString(new_state));
}

}  // namespace aember::aember_init
