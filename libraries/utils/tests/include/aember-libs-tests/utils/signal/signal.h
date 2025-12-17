#pragma once

#include <aember-libs/utils/signal/signal.h>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

class SignalHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    handler_ = std::make_unique<aember::utils::SignalHandler>();
    signal_received_ = false;
    received_signal_ = 0;
  }

  void TearDown() override { handler_->Stop(); }

  std::unique_ptr<aember::utils::SignalHandler> handler_;
  std::atomic<bool> signal_received_;
  int received_signal_;
};
