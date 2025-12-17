#include <aember-libs-tests/utils/signal/signal.h>

#include <csignal>
#include <thread>

namespace aember_test::utils {

using namespace std::chrono_literals;

TEST_F(SignalHandlerTest, RegisterAndReceiveSignal) {
  handler_->Register(SIGUSR1, [this](int sig) {
    signal_received_ = true;
    received_signal_ = sig;
  });

  handler_->Start();

  // Send signal to this process
  std::raise(SIGUSR1);

  // Wait a little for the signal thread to process
  std::this_thread::sleep_for(100ms);

  EXPECT_TRUE(signal_received_);
  EXPECT_EQ(received_signal_, SIGUSR1);
}

TEST_F(SignalHandlerTest, StopPreventsFurtherHandling) {
  handler_->Register(SIGUSR1, [this](int) { signal_received_ = true; });
  handler_->Start();

  handler_->Stop();

  // Send signal after stopping
  signal_received_ = false;
  std::raise(SIGUSR1);
  std::this_thread::sleep_for(100ms);

  EXPECT_FALSE(signal_received_);
}

TEST_F(SignalHandlerTest, MultipleSignalsHandled) {
  std::atomic<int> counter{0};

  handler_->Register(SIGUSR1, [&]([[maybe_unused]] int) { counter++; });
  handler_->Register(SIGUSR2, [&]([[maybe_unused]] int) { counter++; });

  handler_->Start();

  std::raise(SIGUSR1);
  std::raise(SIGUSR2);

  std::this_thread::sleep_for(100ms);

  EXPECT_EQ(counter.load(), 2);
}

}  // namespace aember_test::utils
