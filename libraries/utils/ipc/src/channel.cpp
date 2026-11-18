/**
 * @file channel.cpp
 * @author Arian Ajdari
 * @brief Message channel between PID 1 and its built-in daemons.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/logging/logging.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aember::utils::ipc {

Channel::~Channel() {
  if (fd_ >= 0) ::close(fd_);
}

Channel::Channel(Channel&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), closed_(other.closed_) {}

Channel& Channel::operator=(Channel&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    closed_ = other.closed_;
  }
  return *this;
}

std::expected<std::pair<Channel, Channel>, std::string> Channel::Pair() {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) != 0) {
    return std::unexpected(std::format("socketpair: {}", std::strerror(errno)));
  }
  return std::pair{Channel{fds[0]}, Channel{fds[1]}};
}

Channel Channel::Inherited() {
  struct stat st {};
  if (::fstat(kInheritedFd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
    return Channel{};
  }
  ::fcntl(kInheritedFd, F_SETFD, FD_CLOEXEC);  // don't leak into our children
  return Channel{kInheritedFd};
}

bool Channel::Send(const nlohmann::json& message, bool wait) const {
  if (fd_ < 0) return false;
  const std::string text = message.dump();
  if (text.size() > kMaxMessage) return false;
  // MSG_NOSIGNAL: a dead peer is EPIPE, not a SIGPIPE that kills us.
  const int flags = MSG_NOSIGNAL | (wait ? 0 : MSG_DONTWAIT);
  return ::send(fd_, text.data(), text.size(), flags) ==
         static_cast<ssize_t>(text.size());
}

std::optional<nlohmann::json> Channel::Receive(
    std::chrono::milliseconds timeout) {
  if (fd_ < 0 || closed_) return std::nullopt;

  pollfd p{fd_, POLLIN, 0};
  int r = 0;
  do {
    r = ::poll(&p, 1, static_cast<int>(timeout.count()));
  } while (r < 0 && errno == EINTR);
  if (r <= 0) return std::nullopt;

  std::vector<char> buf(kMaxMessage + 1);
  const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
  if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
    closed_ = true;  // EOF: the peer exited
    return std::nullopt;
  }
  if (n < 0 || static_cast<std::size_t>(n) > kMaxMessage) return std::nullopt;

  auto message = nlohmann::json::parse(
      std::string_view{buf.data(), static_cast<std::size_t>(n)},
      nullptr,
      false);
  if (message.is_discarded() || !message.is_object()) return std::nullopt;
  return message;
}

nlohmann::json Ready() {
  return {{"type", "ready"}};
}

nlohmann::json Event(std::string_view name, nlohmann::json data) {
  return {{"type", "event"}, {"name", name}, {"data", std::move(data)}};
}

std::string TypeOf(const nlohmann::json& message) {
  if (!message.is_object()) return {};
  const auto it = message.find("type");
  return it != message.end() && it->is_string() ? it->get<std::string>() : "";
}

nlohmann::json Console(bool enabled) {
  return {{"type", "console"}, {"enabled", enabled}};
}

bool ApplyControl(const nlohmann::json& message) {
  if (TypeOf(message) != "console") return false;
  const auto it = message.find("enabled");
  if (it == message.end() || !it->is_boolean()) return false;
  aember::utils::logging::set_console_logging(it->get<bool>());
  return true;
}

void ReceiveControl(Channel& channel) {
  if (!channel.Valid()) return;
  while (auto message = channel.Receive(std::chrono::milliseconds{0})) {
    ApplyControl(*message);
  }
}

}  // namespace aember::utils::ipc
