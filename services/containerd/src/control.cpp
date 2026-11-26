/**
 * @file control.cpp
 * @brief containerd's control socket (see control.h).
 */
#include <aember-libs/utils/logging/logging.h>
#include <aember-services/containerd/control.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iterator>
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
    const auto entries = target.List();
    auto list = nlohmann::json::array();
    std::ranges::transform(
        entries, std::back_inserter(list), [](const auto& e) {
          return nlohmann::json{{"name", e.name}, {"state", e.state}};
        });
    return {{"ok", true}, {"containers", std::move(list)}};
  }
  if (op != "stop" && op != "start" && op != "restart" && op != "install" &&
      op != "rollback") {
    return Error("unknown op '" + op + "'");
  }
  if (!request.contains("name") || !request["name"].is_string()) {
    return Error(op + " needs a \"name\"");
  }
  const auto name = request["name"].get<std::string>();
  if (!target.Has(name)) return Error("no container '" + name + "'");
  if (op == "install" || op == "rollback") {
    const auto done =
        op == "install" ? target.Install(name) : target.Rollback(name);
    if (!done) return Error(op + " of '" + name + "' failed: " + done.error());
    return {{"ok", true}};
  }
  bool ok = true;
  if (op == "stop" || op == "restart") ok = target.Stop(name);
  if (ok && (op == "start" || op == "restart")) ok = target.Start(name);
  if (!ok) return Error(op + " of '" + name + "' failed");
  return {{"ok", true}};
}

std::string AnswerControlRequest(std::string_view request,
                                 ControlTarget& target) {
  static aember::utils::logging::Logger log{"containerd-control"};
  const auto parsed = nlohmann::json::parse(request, nullptr, false);
  const auto answer = parsed.is_discarded()
                          ? Error("request is not JSON")
                          : HandleControlRequest(parsed, target);
  // Strings from JSON text are valid UTF-8; replace anyway: an answer is
  // never worth an exception.
  const auto text = [](const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  };
  log.info("control: {} -> {}",
           parsed.is_discarded() ? "?" : text(parsed),
           answer.value("ok", false) ? "ok" : answer.value("error", "?"));
  return text(answer);
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
  const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (fd < 0) return false;
  // A client that connects and says nothing must not stall containerd.
  timeval tv{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  std::array<char, 4096> buf{};
  const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
  const auto out =
      n <= 0 ? Error("no request").dump()
             : AnswerControlRequest(
                   std::string_view{buf.data(), static_cast<std::size_t>(n)},
                   target);
  std::ignore = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
  ::close(fd);
  return true;
}

}  // namespace aember::containerd
