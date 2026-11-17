/**
 * @file hub.cpp
 * @author Arian Ajdari
 * @brief PID 1's side of the daemon channels.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/hub.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace aember::utils::ipc {

Hub::Hub() {
  if (::pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
    log_.error("pipe2: {}", std::strerror(errno));
  }
}

Hub::~Hub() {
  Stop();
  for (const int fd : wake_) {
    if (fd >= 0) ::close(fd);
  }
}

void Hub::Start(MessageHandler on_message, ClosedHandler on_closed) {
  if (thread_.joinable()) return;
  on_message_ = std::move(on_message);
  on_closed_ = std::move(on_closed);
  thread_ = std::jthread{[this](std::stop_token st) {
    try {
      Loop(st);
    } catch (const std::exception& e) {
      log_.error("hub stopped: {}", e.what());
    } catch (...) { log_.error("hub stopped"); }
  }};
}

void Hub::Stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  Wake();
  thread_.join();
}

void Hub::Add(std::string name, Channel channel) {
  {
    std::lock_guard lock{mutex_};
    std::erase_if(entries_, [&](const Entry& e) { return e.name == name; });
    entries_.push_back({std::move(name), std::move(channel)});
  }
  Wake();
}

std::size_t Hub::Size() {
  std::lock_guard lock{mutex_};
  return entries_.size();
}

void Hub::Wake() {
  if (wake_[1] >= 0) {
    const char b = 0;
    std::ignore = ::write(wake_[1], &b, 1);
  }
}

void Hub::Loop(std::stop_token st) {
  while (!st.stop_requested()) {
    std::vector<pollfd> fds{{wake_[0], POLLIN, 0}};
    std::vector<std::string> names;
    {
      std::lock_guard lock{mutex_};
      for (const auto& e : entries_) {
        fds.push_back({e.channel.Fd(), POLLIN, 0});
        names.push_back(e.name);
      }
    }

    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR) continue;
      log_.error("poll: {}", std::strerror(errno));
      return;
    }
    if (fds[0].revents != 0) {  // woken: channels changed or Stop()
      char buf[64];
      while (::read(wake_[0], buf, sizeof(buf)) > 0) {}
      continue;
    }

    for (std::size_t i = 1; i < fds.size(); ++i) {
      if (fds[i].revents == 0) continue;
      const std::string& name = names[i - 1];

      std::optional<nlohmann::json> message;
      bool closed = false;
      {
        std::lock_guard lock{mutex_};
        const auto it = std::ranges::find_if(entries_, [&](const Entry& e) {
          return e.name == name && e.channel.Fd() == fds[i].fd;
        });
        if (it == entries_.end()) continue;  // replaced meanwhile
        message = it->channel.Receive(std::chrono::milliseconds{0});
        closed = it->channel.Closed();
        if (closed) entries_.erase(it);
      }

      // Handlers run without the lock: they may call Add(). A throwing
      // handler costs that one message, never the hub.
      try {
        if (message && on_message_) on_message_(name, *message);
        if (closed) {
          log_.debug("channel '{}' closed", name);
          if (on_closed_) on_closed_(name);
        }
      } catch (const std::exception& e) {
        log_.error("handler for '{}' threw: {}", name, e.what());
      } catch (...) { log_.error("handler for '{}' threw", name); }
    }
  }
}

}  // namespace aember::utils::ipc
