/**
 * @file channel.h
 * @author Arian Ajdari
 * @brief Message channel between PID 1 and its built-in daemons.
 *
 *        A connected SOCK_SEQPACKET socket: every Send() arrives as one
 *        whole message (no framing, no partial reads), and the peer sees
 *        end-of-file when the other side exits or crashes. PID 1 creates a
 *        pair when it starts a daemon and the daemon inherits its end as
 *        fd 3 (kInheritedFd).
 *
 *        Messages are small JSON objects:
 *          {"type": "ready"}                                  daemon is up
 *          {"type": "event", "name": "...", "data": {...}}    state report
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aember::utils::ipc {

/// fd number a daemon finds its channel on.
inline constexpr int kInheritedFd = 3;

/// Largest message (bytes of JSON).
inline constexpr std::size_t kMaxMessage = 64 * 1024;

class Channel {
 public:
  Channel() = default;
  explicit Channel(int fd) : fd_(fd) {}
  ~Channel();

  Channel(Channel&& other) noexcept;
  Channel& operator=(Channel&& other) noexcept;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  /// A connected pair (both ends close-on-exec).
  static std::expected<std::pair<Channel, Channel>, std::string> Pair();

  /// The channel a daemon inherited on fd 3 (made close-on-exec), or an
  /// invalid one if fd 3 isn't a socket.
  static Channel Inherited();

  [[nodiscard]] bool Valid() const { return fd_ >= 0; }
  [[nodiscard]] int Fd() const { return fd_; }

  /// Send one message; false if it is too big or the peer is gone. With
  /// @p wait false it never blocks: a peer that doesn't read (full socket
  /// buffer) makes it fail instead (PID 1 must not hang on a stuck daemon).
  bool Send(const nlohmann::json& message, bool wait = true) const;

  /// Wait up to @p timeout for one message. nullopt on timeout, on a
  /// malformed message, or when the peer closed (then Closed() is true).
  std::optional<nlohmann::json> Receive(std::chrono::milliseconds timeout);

  /// The peer closed its end (seen by Receive).
  [[nodiscard]] bool Closed() const { return closed_; }

  /// Give up ownership of the fd.
  int Release() { return std::exchange(fd_, -1); }

 private:
  int fd_ = -1;
  bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

/// {"type": "ready"}
nlohmann::json Ready();

/// {"type": "event", "name": name, "data": data}
nlohmann::json Event(std::string_view name, nlohmann::json data);

/// "type" of a message, "" if it has none.
std::string TypeOf(const nlohmann::json& message);

/// PID 1 -> daemon: {"type": "console", "enabled": enabled}. Sent while a
/// debug shell owns the console (false) and when it is closed (true).
nlohmann::json Console(bool enabled);

/// Daemon side: apply a message from PID 1 (today: "console" mutes or
/// unmutes the console log). False if it isn't a control message.
bool ApplyControl(const nlohmann::json& message);

/// Daemon side: apply every message waiting on @p channel, without
/// blocking. Call it right after Inherited() (PID 1 queues its state
/// before the daemon starts) and whenever the channel is readable.
void ReceiveControl(Channel& channel);

}  // namespace aember::utils::ipc
