#include <aember-libs/utils/signal/signal.h>

#include <pthread.h>

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
  if (running_.exchange(true)) return;

  // Block signals in all threads
  pthread_sigmask(SIG_BLOCK, &signal_set_, nullptr);

  signal_thread_ = std::thread([this] { SignalLoop(); });
}

void SignalHandler::Stop() {
  if (!running_.exchange(false)) return;

  // Wake sigwait()
  pthread_kill(signal_thread_.native_handle(), SIGTERM);

  if (signal_thread_.joinable()) { signal_thread_.join(); }
}

void SignalHandler::SignalLoop() {
  while (running_.load()) {
    int sig = 0;
    if (sigwait(&signal_set_, &sig) != 0) { continue; }

    auto it = callbacks_.find(sig);
    if (it != callbacks_.end()) { it->second(sig); }
  }
}

}  // namespace aember::utils
