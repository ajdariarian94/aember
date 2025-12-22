/**
 * @file signal.h
 * @author Arian Ajdari
 * @brief Library definition for signal handling in Aember
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <signal.h>

#include <atomic>
#include <functional>
#include <map>
#include <thread>

namespace aember::utils {

/**
 * @brief Lightweight signal handler for registering and handling POSIX signals.
 *
 * Provides a dedicated thread to wait for signals and invoke registered callbacks.
 */
class SignalHandler {
 public:
  using Callback = std::function<void(int)>;

  /**
   * @brief Constructs a SignalHandler object.
   *
   * Initializes internal data structures for signal handling.
   */
  SignalHandler();

  /**
   * @brief Destructor stops signal handling and joins the thread.
   */
  ~SignalHandler();

  // Non-copyable
  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;

  /**
   * @brief Register a callback for a specific signal.
   *
   * @param signal Signal number (e.g., SIGINT, SIGTERM)
   * @param cb Function to call when the signal is received
   */
  void Register(int signal, Callback cb);

  /**
   * @brief Start the signal handling loop in a separate thread.
   *
   * Must be called once after registering callbacks.
   */
  void Start();

  /**
   * @brief Stop signal handling and join the internal thread.
   *
   * Safe to call multiple times.
   */
  void Stop();

 private:
  /**
   * @brief Internal loop that waits for signals and dispatches callbacks.
   */
  void SignalLoop();

  sigset_t signal_set_;                  ///< Mask of signals to handle
  std::map<int, Callback> callbacks_;    ///< Registered callbacks for signals
  std::atomic<bool> running_{false};     ///< Thread running state
  std::thread signal_thread_;            ///< Thread for handling signals
};

}  // namespace aember::utils
