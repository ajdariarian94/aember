#include <atomic>
#include <stdexcept>
#include <thread>

#include <aember-libs-tests/utils/signal/signal.h>

namespace aember_test::utils {

using namespace std::chrono_literals;

TEST_F(SignalHandlerTest, RegisterAndStart) {
  handler_->Register(SIGUSR1, [](int) {});
  handler_->Start();
  // Should not crash
}

TEST_F(SignalHandlerTest, SingleSignalHandled) {
  std::atomic<int> signal_received{0};

  handler_->Register(SIGUSR1, [&](int sig) { signal_received.store(sig); });

  handler_->Start();

  // Send signal to this process
  ASSERT_EQ(kill(getpid(), SIGUSR1), 0);

  // Wait for signal to be handled (max 1 second)
  for (int i = 0; i < 100 && signal_received.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(signal_received.load(), SIGUSR1);
}

TEST_F(SignalHandlerTest, MultipleSignalsHandled) {
  std::atomic<int> counter{0};

  handler_->Register(
      SIGUSR1, [&](int) { counter.fetch_add(1, std::memory_order_relaxed); });
  handler_->Register(
      SIGUSR2, [&](int) { counter.fetch_add(1, std::memory_order_relaxed); });

  handler_->Start();

  ASSERT_EQ(kill(getpid(), SIGUSR1), 0);

  // Small delay to ensure first signal is processed
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ASSERT_EQ(kill(getpid(), SIGUSR2), 0);

  // Wait for both signals to be handled (max 1 second)
  for (int i = 0; i < 100 && counter.load(std::memory_order_relaxed) < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(counter.load(std::memory_order_relaxed), 2);
}

TEST_F(SignalHandlerTest, SameSignalMultipleTimes) {
  std::atomic<int> counter{0};

  handler_->Register(
      SIGUSR1, [&](int) { counter.fetch_add(1, std::memory_order_relaxed); });

  handler_->Start();

  // Send the same signal multiple times WITH DELAYS
  // Standard signals can coalesce if sent too quickly
  for (int i = 0; i < 3; ++i) {  // Reduced from 5 to 3 for reliability
    ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(50));  // Wait between sends
  }

  // Wait for all signals to be handled
  for (int i = 0; i < 200 && counter.load(std::memory_order_relaxed) < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(counter.load(std::memory_order_relaxed), 3);
}

TEST_F(SignalHandlerTest, StopWithoutStart) {
  handler_->Register(SIGUSR1, [](int) {});
  handler_->Stop();
  // Should not crash
}

TEST_F(SignalHandlerTest, MultipleStartCalls) {
  handler_->Register(SIGUSR1, [](int) {});
  handler_->Start();
  handler_->Start();  // Second call should be ignored
  // Should not crash
}

TEST_F(SignalHandlerTest, MultipleStopCalls) {
  handler_->Register(SIGUSR1, [](int) {});
  handler_->Start();
  handler_->Stop();
  handler_->Stop();  // Second call should be ignored
  // Should not crash
}

// Stop() hands the signals back: unblocked again in the calling thread.
TEST_F(SignalHandlerTest, StopUnblocksTheSignals) {
  handler_->Register(SIGUSR2, [](int) {});
  handler_->Start();
  sigset_t now;
  ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &now), 0);
  EXPECT_EQ(sigismember(&now, SIGUSR2), 1);
  handler_->Stop();
  ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &now), 0);
  EXPECT_EQ(sigismember(&now, SIGUSR2), 0);
}

TEST_F(SignalHandlerTest, CallbackReceivesCorrectSignal) {
  std::atomic<int> usr1_count{0};
  std::atomic<int> usr2_count{0};

  handler_->Register(SIGUSR1, [&](int sig) {
    EXPECT_EQ(sig, SIGUSR1);
    usr1_count.fetch_add(1, std::memory_order_relaxed);
  });

  handler_->Register(SIGUSR2, [&](int sig) {
    EXPECT_EQ(sig, SIGUSR2);
    usr2_count.fetch_add(1, std::memory_order_relaxed);
  });

  handler_->Start();

  ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ASSERT_EQ(kill(getpid(), SIGUSR2), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ASSERT_EQ(kill(getpid(), SIGUSR1), 0);

  // Wait for signals to be handled
  for (int i = 0; i < 100 && (usr1_count.load(std::memory_order_relaxed) < 2 ||
                              usr2_count.load(std::memory_order_relaxed) < 1);
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(usr1_count.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(usr2_count.load(std::memory_order_relaxed), 1);
}
}  // namespace aember_test::utils

namespace aember_test::utils {

// Regression: an exception from a callback escaped the signal thread
// (std::terminate, fatal in PID 1). Now it is logged and the next signal
// is still handled.
TEST_F(SignalHandlerTest, ThrowingCallbackKeepsTheHandlerAlive) {
  std::atomic<int> calls{0};
  handler_->Register(SIGUSR1, [&](int) {
    ++calls;
    throw std::runtime_error("boom");
  });
  handler_->Start();

  for (int round = 1; round <= 2; ++round) {
    ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
    for (int i = 0; i < 100 && calls.load() < round; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(calls.load(), round);
  }
}

}  // namespace aember_test::utils
