/**
 * @file control.cpp
 * @brief containerd's control socket (see control.h).
 */
#include <aember-libs/utils/logging/logging.h>
#include <aember-services/containerd/control.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace aember::containerd {

namespace {

nlohmann::json Error(std::string message) {
  return {{"ok", false}, {"error", std::move(message)}};
}

}  // namespace

nlohmann::json HandleControlRequest(const nlohmann::json& request,
                                    ControlTarget& target) {
  if (!request.is_object() || !request.contains("op") ||
      !request["op"].is_string()) {
    return Error("want {\"op\": ...}");
  }
  const auto op = request["op"].get<std::string>();
  if (op == "list") {
    auto list = nlohmann::json::array();
    for (const auto& e : target.List()) {
      list.push_back({{"name", e.name}, {"state", e.state}});
    }
    return {{"ok", true}, {"containers", std::move(list)}};
  }
  if (op != "stop" && op != "start" && op != "restart") {
    return Error("unknown op '" + op + "'");
  }
  if (!request.contains("name") || !request["name"].is_string()) {
    return Error(op + " needs a \"name\"");
  }
  const auto name = request["name"].get<std::string>();
  if (!target.Has(name)) return Error("no container '" + name + "'");
  bool ok = true;
  if (op == "stop" || op == "restart") ok = target.Stop(name);
  if (ok && (op == "start" || op == "restart")) ok = target.Start(name);
  if (!ok) return Error(op + " of '" + name + "' failed");
  return {{"ok", true}};
}

int OpenControlSocket(const std::string& path) {
  sockaddr_un addr{};
  if (path.size() >= sizeof(addr.sun_path)) return -ENAMETOOLONG;
  std::error_code ec;
  const auto dir = std::filesystem::path{path}.parent_path();
  std::filesystem::create_directories(dir, ec);
  ::chmod(dir.c_str(), 0700);
  ::unlink(path.c_str());

  const int fd =
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -errno;
  addr.sun_family = AF_UNIX;
  path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 8) != 0) {
    const int err = errno;
    ::close(fd);
    return -err;
  }
  return fd;
}

bool ServeControlConnection(int listen_fd, ControlTarget& target) {
  static aember::utils::logging::Logger log{"containerd-control"};
  const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (fd < 0) return false;
  // A client that connects and says nothing must not stall containerd.
  timeval tv{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  std::array<char, 4096> buf{};
  const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
  nlohmann::json answer;
  if (n <= 0) {
    answer = Error("no request");
  } else {
    const auto request = nlohmann::json::parse(
        std::string_view{buf.data(), static_cast<std::size_t>(n)},
        nullptr,
        false);
    answer = request.is_discarded() ? Error("request is not JSON")
                                    : HandleControlRequest(request, target);
    log.info("control: {} -> {}",
             request.is_discarded() ? "?" : request.dump(),
             answer.value("ok", false) ? "ok" : answer.value("error", "?"));
  }
  const auto out = answer.dump();
  std::ignore = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
  ::close(fd);
  return true;
}

}  // namespace aember::containerd
