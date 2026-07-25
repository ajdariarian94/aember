/**
 * @file signal.h
 * @author Arian Ajdari
 * @brief Robust POSIX signal handler using signalfd + epoll.
 *
 *        Uses Linux signalfd(2) to convert signals into file descriptor
 *        read events — eliminating signal coalescing issues that affect
 *        sigwait-based handlers, especially for SIGCHLD in a PID1 context.
 *
 *        C++26: std::move_only_function, std::jthread, std::stop_token.
 *
 * @version 0.2
 * @date 2026-07-25
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <atomic>
#include <functional>
#include <map>
#include <thread>

#include <signal.h>

namespace aember::utils::signal {

/**
 * @brief Signal handler backed by signalfd + epoll.
 *
 * All registered signals are blocked in every thread via pthread_sigmask,
 * then read as structured events from a signalfd file descriptor.
 * This guarantees every signal delivery is visible — no coalescing,
 * no lost SIGCHLD.
 */
class SignalHandler {
 public:
  using Callback = std::move_only_function<void(int)>;

  SignalHandler();
  ~SignalHandler();

  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;

  /**
   * @brief Register a callback for a signal.
   *        Must be called before Start().
   */
  void Register(int signal, Callback cb);

  /**
   * @brief Block all registered signals and start the signalfd loop.
   */
  void Start();

  /**
   * @brief Stop the loop and join the thread.
   */
  void Stop();

 private:
  void SignalLoop(std::stop_token stop);

  sigset_t signal_set_{};
  std::map<int, Callback> callbacks_;
  std::atomic<bool> running_{false};
  int signal_fd_{-1};
  int epoll_fd_{-1};
  int wake_pipe_[2]{-1, -1};  ///< used to unblock epoll on Stop()
  std::jthread signal_thread_;

  using Logger = aember::utils::logging::Logger;
  mutable Logger log_{"signal-handler"};
};

}  // namespace aember::utils::signal
