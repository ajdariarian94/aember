/**
 * @file signal.cpp
 * @author Arian Ajdari
 * @brief Library implementation for signal handling in Aember
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/signal/signal.h>

#include <pthread.h>
#include <csignal>
#include <iostream>

namespace aember::utils::signal {

SignalHandler::SignalHandler() {
  // Initialize empty signal set
  sigemptyset(&signal_set_);
}

SignalHandler::~SignalHandler() {
  // Ensure the signal handling thread is stopped before destruction
  Stop();
}

void SignalHandler::Register(int signal, Callback cb) {
  // Register a callback for the given signal
  callbacks_[signal] = std::move(cb);

  // Add signal to the internal mask
  sigaddset(&signal_set_, signal);
}

void SignalHandler::Start() {
  // Start signal handling thread if not already running
  if (running_.exchange(true, std::memory_order_acquire)) return;

  // Block signals in the current thread; new thread inherits this mask
  pthread_sigmask(SIG_BLOCK, &signal_set_, nullptr);

  // Launch thread to handle signals
  signal_thread_ = std::thread([this] { SignalLoop(); });
}

void SignalHandler::Stop() {
  // Stop signal handling thread
  if (!running_.exchange(false, std::memory_order_release)) return;

  // Wake up the sigwait() by sending a registered signal
  if (!callbacks_.empty() && signal_thread_.joinable()) {
    int sig = callbacks_.begin()->first;
    pthread_kill(signal_thread_.native_handle(), sig);
  }

  // Wait for the thread to finish
  if (signal_thread_.joinable()) { signal_thread_.join(); }

  // Unblock signals in the current thread after stopping
  pthread_sigmask(SIG_UNBLOCK, &signal_set_, nullptr);
}

void SignalHandler::SignalLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    int sig = 0;

    // Wait for a signal in the signal set
    int result = sigwait(&signal_set_, &sig);

    if (result != 0) {
      continue;  // sigwait error, retry
    }

    // Exit loop if handler stopped
    if (!running_.load(std::memory_order_relaxed)) { break; }

    // Call registered callback if available
    auto it = callbacks_.find(sig);
    if (it != callbacks_.end()) { it->second(sig); }
  }
}

}  // namespace aember::utils::signal
