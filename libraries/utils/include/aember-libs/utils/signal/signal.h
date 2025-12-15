#pragma once

#include <signal.h>

#include <atomic>
#include <functional>
#include <map>
#include <thread>

namespace aember::utils {

class SignalHandler {
 public:
  using Callback = std::function<void(int)>;

  SignalHandler();
  ~SignalHandler();

  // Non-copyable
  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;

  // Register a callback for a signal
  void Register(int signal, Callback cb);

  // Start handling signals (call once)
  void Start();

  // Stop signal handling and join thread
  void Stop();

 private:
  void SignalLoop();

  sigset_t signal_set_;
  std::map<int, Callback> callbacks_;

  std::atomic<bool> running_{false};
  std::thread signal_thread_;
};

}  // namespace aember::utils
