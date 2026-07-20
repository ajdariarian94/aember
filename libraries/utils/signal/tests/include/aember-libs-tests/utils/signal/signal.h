#pragma once

#include <aember-libs/utils/signal/signal.h>

#include <atomic>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include <signal.h>

namespace aember_test::utils {

class SignalHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Save the original signal mask to restore later
    pthread_sigmask(SIG_SETMASK, nullptr, &original_mask_);

    handler_ = std::make_unique<aember::utils::SignalHandler>();
  }

  void TearDown() override {
    if (handler_) {
      handler_->Stop();
      handler_.reset();
    }

    // Restore the original signal mask
    pthread_sigmask(SIG_SETMASK, &original_mask_, nullptr);
  }

  sigset_t original_mask_{};
  std::unique_ptr<aember::utils::SignalHandler> handler_;
};

}  // namespace aember_test::utils
