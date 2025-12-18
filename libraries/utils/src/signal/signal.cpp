#include <aember-libs/utils/signal/signal.h>

#include <pthread.h>
#include <csignal>
#include <iostream>

namespace aember::utils {

SignalHandler::SignalHandler() {
  sigemptyset(&signal_set_);
}

SignalHandler::~SignalHandler() {
  Stop();
}

void SignalHandler::Register(int signal, Callback cb) {
  callbacks_[signal] = std::move(cb);
  sigaddset(&signal_set_, signal);
}

void SignalHandler::Start() {
  if (running_.exchange(true, std::memory_order_acquire)) return;

  // Block signals in the calling thread BEFORE creating the handler thread
  // The new thread will inherit this blocked signal mask
  pthread_sigmask(SIG_BLOCK, &signal_set_, nullptr);

  signal_thread_ = std::thread([this] { SignalLoop(); });
}

void SignalHandler::Stop() {
  if (!running_.exchange(false, std::memory_order_release)) return;

  // Wake up sigwait() by sending one of the registered signals to the thread
  if (!callbacks_.empty() && signal_thread_.joinable()) {
    int sig = callbacks_.begin()->first;
    pthread_kill(signal_thread_.native_handle(), sig);
  }

  if (signal_thread_.joinable()) { signal_thread_.join(); }

  // Unblock the signals after stopping
  pthread_sigmask(SIG_UNBLOCK, &signal_set_, nullptr);
}

void SignalHandler::SignalLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    int sig = 0;
    int result = sigwait(&signal_set_, &sig);

    if (result != 0) {
      continue;  // Error in sigwait, try again
    }

    // Check if we're still running (might have been woken up to stop)
    if (!running_.load(std::memory_order_relaxed)) { break; }

    auto it = callbacks_.find(sig);
    if (it != callbacks_.end()) { it->second(sig); }
  }
}

}  // namespace aember::utils
