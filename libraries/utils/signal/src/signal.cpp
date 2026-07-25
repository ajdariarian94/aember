/**
 * @file signal.cpp
 * @author Arian Ajdari
 * @brief Robust signal handler using signalfd + epoll.
 *
 *        signalfd converts each signal delivery into a read event on a
 *        file descriptor. epoll_wait monitors both the signalfd and a
 *        wake pipe so Stop() is instant without sending a dummy signal.
 *
 * @version 0.2
 * @date 2026-07-25
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/signal/signal.h>

#include <format>
#include <stdexcept>

#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace aember::utils::signal {

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

SignalHandler::SignalHandler() {
  sigemptyset(&signal_set_);

  // Create wake pipe — written to by Stop() to unblock epoll_wait
  if (::pipe(wake_pipe_) != 0) {
    throw std::runtime_error(
        std::format("SignalHandler: pipe() failed: errno={}", errno));
  }
}

SignalHandler::~SignalHandler() {
  Stop();

  if (epoll_fd_ >= 0) { ::close(epoll_fd_); }
  if (signal_fd_ >= 0) { ::close(signal_fd_); }
  if (wake_pipe_[0] >= 0) { ::close(wake_pipe_[0]); }
  if (wake_pipe_[1] >= 0) { ::close(wake_pipe_[1]); }
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

void SignalHandler::Register(int sig, Callback cb) {
  callbacks_[sig] = std::move(cb);
  sigaddset(&signal_set_, sig);
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

void SignalHandler::Start() {
  if (running_.exchange(true)) return;

  // Block all registered signals in every thread — signalfd reads them instead
  pthread_sigmask(SIG_BLOCK, &signal_set_, nullptr);

  // Create signalfd — delivers blocked signals as readable events
  signal_fd_ = ::signalfd(-1, &signal_set_, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signal_fd_ < 0) {
    throw std::runtime_error(
        std::format("SignalHandler: signalfd() failed: errno={}", errno));
  }

  // Create epoll instance
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::runtime_error(
        std::format("SignalHandler: epoll_create1() failed: errno={}", errno));
  }

  // Watch signalfd
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = signal_fd_;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &ev);

  // Watch wake pipe read end
  ev.data.fd = wake_pipe_[0];
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_pipe_[0], &ev);

  log_.info(
      "SignalHandler started (signalfd={} epoll={})", signal_fd_, epoll_fd_);

  // Launch jthread — stop_token requested by Stop() via request_stop()
  signal_thread_ =
      std::jthread{[this](std::stop_token st) { SignalLoop(std::move(st)); }};
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void SignalHandler::Stop() {
  if (!running_.exchange(false)) return;

  signal_thread_.request_stop();

  // Write to wake pipe to unblock epoll_wait immediately
  char byte = 0;
  ::write(wake_pipe_[1], &byte, 1);

  if (signal_thread_.joinable()) { signal_thread_.join(); }

  pthread_sigmask(SIG_UNBLOCK, &signal_set_, nullptr);

  log_.info("SignalHandler stopped");
}

// ---------------------------------------------------------------------------
// SignalLoop
// ---------------------------------------------------------------------------

void SignalHandler::SignalLoop(std::stop_token stop) {
  constexpr int kMaxEvents = 8;
  epoll_event events[kMaxEvents];

  while (!stop.stop_requested()) {
    const int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, -1);

    if (n < 0) {
      if (errno == EINTR) continue;
      log_.error("epoll_wait failed: errno={}", errno);
      break;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;

      if (fd == wake_pipe_[0]) {
        // Stop() woke us up — drain the pipe and exit
        char buf[64];
        ::read(wake_pipe_[0], buf, sizeof(buf));
        return;
      }

      if (fd == signal_fd_) {
        // Drain all pending signals from signalfd
        signalfd_siginfo info{};
        while (::read(signal_fd_, &info, sizeof(info)) == sizeof(info)) {
          const int sig = static_cast<int>(info.ssi_signo);
          log_.debug("Signal {} received (pid={})", sig, info.ssi_pid);

          if (auto it = callbacks_.find(sig); it != callbacks_.end()) {
            it->second(sig);
          }
        }
      }
    }
  }
}

}  // namespace aember::utils::signal
