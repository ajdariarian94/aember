/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of the AemberInit init system.
 */

#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/device-health/aember-monitor.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <stdexcept>
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
    : options_(options), log_(std::string{logger_name}) {
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

void AemberInit::StartRoot() {
  if (running_.exchange(true)) return;

  aember::utils::logging::enable_file_logging("/var/log/aember-init.log");
  log_.info("Starting Aember in root mode{}",
            options_.chroot ? " (chroot)" : "");

  // Create heartbeat JSON file early — needed for aember-monitor container
  // mount
  {
    std::ofstream file{"/var/log/aember-monitor.json"};
    file << "{}";
  }

  Guarded(log_, "Mount phase", [this] {
    InitMounts().or_else([this](const Error& e) -> Result<> {
      log_.warn("Mount phase failed: {}", e.message);
      return {};
    });
  });

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

  // From here on 'd' on the console opens a shell at any time; boot does not
  // wait for it. aember.shell=1 on the kernel command line opens one now.
  StartDebugShell(DebugShell::RequestedOnCmdline());

  if (service_manager_) {
    Guarded(log_, "Loading services", [this] {
      LoadAndRegisterProcesses("/etc/aember/services.json")
          .or_else([this](const Error& e) -> Result<> {
            log_.warn("{}", e.message);
            return {};
          });
    });

    // Network and containers run as built-in daemons (`aember netd`,
    // `aember containerd`), supervised like any service.
    Guarded(log_, "Built-in daemons", [this] { RegisterDaemons(); });

    Guarded(
        log_, "Starting services", [this] { service_manager_->StartAll(); });
  } else {
    log_.error("No service manager: no services, network or containers");
  }

  Guarded(log_, "Monitor and heartbeat", [this] {
    // AemberMonitor reads PID1 stats from /proc.
    monitor_ = std::make_unique<aember::device_health::AemberMonitor>();
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
  monitor_.reset();

  // Destroy in reverse construction order.
  service_manager_.reset();
  dependency_resolver_.reset();
  process_manager_.reset();
  mount_manager_.reset();

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
  auto modules = module_loader_->LoadModules("/etc/aember/modules.json");
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

void AemberInit::RegisterDaemons() {
  // Same binary, another role (multi-call): /proc/self/exe is this aember.
  std::error_code ec;
  auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) { self = "/usr/bin/aember"; }

  const auto daemon = [&](std::string name,
                          std::string role,
                          std::vector<std::string>
                              deps,
                          std::chrono::milliseconds stop_timeout) {
    aember::utils::process::ProcessConfig cfg;
    cfg.name = std::move(name);
    cfg.description = "built-in " + role + " daemon";
    cfg.executable = self.string();
    cfg.args = {std::move(role)};
    cfg.dependencies = std::move(deps);
    cfg.ipc = true;
    cfg.ready_timeout = std::chrono::seconds{120};  // DHCP retries take time
    cfg.restart_on_failure = true;
    cfg.max_restarts = 0;  // always
    cfg.restart_delay = std::chrono::seconds{2};
    cfg.stop_timeout = stop_timeout;
    if (!process_manager_->AddProcess(cfg) ||
        !service_manager_->AddProcess(cfg.name)) {
      log_.error("Could not register built-in daemon '{}'", cfg.name);
    }
  };

  daemon(std::string{kNetd}, "netd", {}, std::chrono::seconds{10});
  // Containers need the bridge; stopping them gracefully takes a while.
  daemon(std::string{kContainerd},
         "containerd",
         {std::string{kNetd}},
         std::chrono::seconds{60});
}

void AemberInit::OnDaemonMessage(const std::string& name,
                                 const nlohmann::json& message) {
  namespace ipc = aember::utils::ipc;
  const auto type = ipc::TypeOf(message);
  if (type == "ready") {
    if (service_manager_) { service_manager_->MarkReady(name); }
    return;
  }
  if (type != "event" || !message.contains("data")) { return; }

  const auto& data = message["data"];
  if (!data.is_object()) { return; }
  const auto online = [](const nlohmann::json& j) {
    const auto it = j.find("online");
    return j.is_object() && it != j.end() && it->is_boolean() &&
           it->get<bool>();
  };
  const auto text = [&](const char* key) {
    const auto it = data.find(key);
    return it != data.end() && it->is_string() ? it->get<std::string>()
                                               : std::string{};
  };

  const auto name_it = message.find("name");
  const std::string event = name_it != message.end() && name_it->is_string()
                                ? name_it->get<std::string>()
                                : "";
  std::lock_guard lock{status_mutex_};
  if (event == "network") {
    const bool was_online = online(network_status_);
    network_status_ = data;
    if (was_online && !online(network_status_)) {
      log_.warn("Network: internet connectivity lost");
    }
  } else if (event == "container") {
    const auto container = text("name");
    const auto state = text("state");
    if (!container.empty()) {
      log_.info("Container '{}': {}", container, state);
      container_states_[container] = state;
    }
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
  auto configs = process_manager_->LoadProcesses(std::string{path});
  log_.info("Loaded {} process config(s) from {}", configs.size(), path);

  const auto registered =
      configs | std::views::filter([&](const auto& cfg) {
        if (!process_manager_->AddProcess(cfg)) {
          log_.error("Failed to register process '{}'", cfg.name);
          return false;
        }
        return true;
      }) |
      std::views::transform([&](const auto& cfg) { return cfg.name; }) |
      std::ranges::to<std::vector>();

  std::ranges::for_each(registered, [&](const auto& name) {
    if (!service_manager_->AddProcess(name)) {
      log_.error("Failed to add process '{}' to service manager", name);
    } else {
      log_.info("Registered process '{}'", name);
    }
  });

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
    // The reaper collects the shell: track it, and it is alive until reaped.
    debug_shell_->Start(
        [this](pid_t pid) { child_supervisor_.AddChild(pid, "debug-shell"); },
        [this](pid_t pid) { return child_supervisor_.IsTracked(pid); },
        shell_now);
  });
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
  while (running_.load() && shutdown_.load() == ShutdownAction::kNone) {
    cv_.wait_for(lock, std::chrono::seconds(1));
  }
  log_.info("Aember Init run loop exiting");
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void AemberInit::OnHeartbeat(const nlohmann::json& base) {
  if (!monitor_) return;

  // Collect PID1 stats via /proc.
  auto payload = monitor_->Snapshot();

  // Network status and container states as the daemons last reported them.
  nlohmann::json containers = nlohmann::json::array();
  {
    std::lock_guard lock{status_mutex_};
    if (!network_status_.is_null()) { payload["network"] = network_status_; }
    for (const auto& [name, state] : container_states_) {
      containers.push_back(
          {{"name", name}, {"state", state}, {"type", "container"}});
    }
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
    }
    for (auto& c : containers) { services.push_back(std::move(c)); }
    payload["services"] = services;
  }

  // Merge heartbeat base fields (timestamp, status).
  payload.merge_patch(base);

  // Log the enriched dashboard.
  monitor_->Log(payload);
  monitor_->LogToFile(payload, "/var/log/aember-monitor.json");
}

void AemberInit::OnServiceStateChange(const std::string& name,
                                      ProcessState old_state,
                                      ProcessState new_state) {
  log_.info("Service '{}': {} -> {}",
            name,
            aember::utils::process::ToString(old_state),
            aember::utils::process::ToString(new_state));
}

}  // namespace aember::aember_init
