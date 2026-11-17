/**
 * @file hub.h
 * @author Arian Ajdari
 * @brief PID 1's side of the daemon channels: one thread that polls every
 *        channel and hands each message to a handler.
 *
 *        Channels are added while it runs (a restarted daemon brings a new
 *        one) and removed when their peer closes. Handlers run on the hub
 *        thread; they must not block for long.
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aember::utils::ipc {

class Hub {
 public:
  /// (channel name, message)
  using MessageHandler =
      std::move_only_function<void(const std::string&, const nlohmann::json&)>;
  /// (channel name) — the peer closed (daemon exited)
  using ClosedHandler = std::move_only_function<void(const std::string&)>;

  Hub();
  ~Hub();

  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  void Start(MessageHandler on_message, ClosedHandler on_closed = nullptr);
  void Stop();

  /// Watch @p channel under @p name (replaces a channel of the same name).
  void Add(std::string name, Channel channel);

  /// Number of channels being watched.
  [[nodiscard]] std::size_t Size();

 private:
  void Loop(std::stop_token st);
  void Wake();

  struct Entry {
    std::string name;
    Channel channel;
  };

  std::mutex mutex_;
  std::vector<Entry> entries_;
  int wake_[2] = {-1, -1};
  MessageHandler on_message_;
  ClosedHandler on_closed_;
  std::jthread thread_;
  aember::utils::logging::Logger log_{"ipc-hub"};
};

}  // namespace aember::utils::ipc
