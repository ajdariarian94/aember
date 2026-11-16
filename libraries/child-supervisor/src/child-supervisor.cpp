/**
 * @file child-supervisor.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of ChildSupervisor.
 */

#include <aember-libs/child-supervisor/child-supervisor.h>

#include <charconv>
#include <fstream>
#include <unordered_set>

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aember::child_supervisor {

// ---------------------------------------------------------------------------
// Ctor
// ---------------------------------------------------------------------------

ChildSupervisor::ChildSupervisor() {
  log_.info("ChildSupervisor initialized");
}

// ---------------------------------------------------------------------------
// Child tracking
// ---------------------------------------------------------------------------

void ChildSupervisor::AddChild(pid_t pid, std::string_view name) {
  std::lock_guard lock{children_mutex_};
  children_.emplace(pid, std::string{name});
  log_.info("Tracking child '{}' (pid={})", name, pid);
}

void ChildSupervisor::RemoveChild(pid_t pid) {
  std::lock_guard lock{children_mutex_};

  auto it = children_.find(pid);
  if (it == children_.end()) {
    log_.debug("RemoveChild: unknown pid={}", pid);
    return;
  }

  log_.info("Removed child '{}' (pid={})", it->second, pid);
  children_.erase(it);
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

void ChildSupervisor::HandleSIGCHLD() {
  log_.debug("SIGCHLD received");
  ReapChildren();
}

void ChildSupervisor::ReapChildren() {
  // Interpret waitpid status into a log-friendly string.
  const auto describe_exit = [](int status) -> std::string {
    if (WIFEXITED(status)) {
      return std::format("exited with code {}", WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
      return std::format("killed by signal {}", WTERMSIG(status));
    }
    return "exited with unknown status";
  };

  while (true) {
    int status = 0;
    const pid_t pid = ::waitpid(-1, &status, WNOHANG);

    if (pid == 0) { break; }  // no more exited children

    if (pid < 0) {
      if (errno != ECHILD) { log_.error("waitpid failed: errno={}", errno); }
      break;
    }

    // Look up name and remove from tracking.
    std::string name = "<unknown>";
    {
      std::lock_guard lock{children_mutex_};
      if (auto it = children_.find(pid); it != children_.end()) {
        name = std::move(it->second);
        children_.erase(it);
      }
    }

    log_.info("Child '{}' (pid={}) {}", name, pid, describe_exit(status));
  }
}

// ---------------------------------------------------------------------------
// ReapZombies (PID 1)
// ---------------------------------------------------------------------------

bool ChildSupervisor::IsTracked(pid_t pid) {
  std::lock_guard lock{children_mutex_};
  return children_.contains(pid);
}

std::vector<pid_t> ChildSupervisor::ListZombieChildren(
    const std::filesystem::path& proc_root) {
  std::vector<pid_t> zombies;
  const pid_t self = ::getpid();

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator{proc_root, ec}) {
    const auto name = entry.path().filename().string();
    pid_t pid = 0;
    const auto [end, err] =
        std::from_chars(name.data(), name.data() + name.size(), pid);
    if (err != std::errc{} || end != name.data() + name.size()) { continue; }

    // "<pid> (<comm>) <state> <ppid> ..." — comm may contain spaces and
    // parentheses, so parse from the last ')'.
    std::ifstream stat{entry.path() / "stat"};
    std::string line;
    if (!std::getline(stat, line)) { continue; }
    const auto close = line.rfind(')');
    if (close == std::string::npos || close + 2 >= line.size()) { continue; }

    char state = 0;
    pid_t ppid = 0;
    const char* p = line.c_str() + close + 2;
    state = *p;
    const char* ppid_begin = p + 2;
    const char* line_end = line.c_str() + line.size();
    if (ppid_begin >= line_end) { continue; }
    std::from_chars(ppid_begin, line_end, ppid);

    if (state == 'Z' && ppid == self) { zombies.push_back(pid); }
  }
  return zombies;
}

std::vector<ChildSupervisor::Exit> ChildSupervisor::ReapZombies(
    std::chrono::milliseconds grace, const std::filesystem::path& proc_root) {
  std::vector<Exit> exits;
  const auto now = std::chrono::steady_clock::now();
  const auto zombies = ListZombieChildren(proc_root);

  std::lock_guard lock{children_mutex_};

  // Forget untracked zombies that disappeared (their waiter reaped them).
  const std::unordered_set<pid_t> present{zombies.begin(), zombies.end()};
  std::erase_if(untracked_seen_,
                [&](const auto& kv) { return !present.contains(kv.first); });

  for (const pid_t pid : zombies) {
    const auto tracked = children_.find(pid);
    if (tracked == children_.end()) {
      const auto [it, inserted] = untracked_seen_.try_emplace(pid, now);
      if (now - it->second < grace) { continue; }
    }

    int status = 0;
    // A zombie can't be reused as a pid, so waiting for this exact pid is
    // safe; WNOHANG because a synchronous waiter may have just taken it.
    if (::waitpid(pid, &status, WNOHANG) != pid) {
      untracked_seen_.erase(pid);
      continue;
    }

    Exit exit{.pid = pid, .status = status, .tracked = false, .name = {}};
    if (tracked != children_.end()) {
      exit.tracked = true;
      exit.name = std::move(tracked->second);
      children_.erase(tracked);
    }
    untracked_seen_.erase(pid);
    exits.push_back(std::move(exit));
  }

  return exits;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ChildSupervisor::StopAll() {
  std::lock_guard lock{children_mutex_};

  if (!children_.empty()) {
    log_.warn("StopAll: {} child(ren) still tracked at shutdown",
              children_.size());

    for (const auto& [pid, name] : children_) {
      log_.warn("  '{}' (pid={})", name, pid);
    }
  }

  children_.clear();
  log_.info("ChildSupervisor stopped");
}

}  // namespace aember::child_supervisor
