/**
 * @file control.h
 * @brief containerd's control socket: other services (the aember-fleet
 *        agent) list, stop, start and restart containers through it.
 *
 * A SOCK_SEQPACKET Unix socket (default /run/aember-containerd/control.sock,
 * directory 0700): one JSON request per connection, one JSON answer.
 *
 *   {"op": "list"}              -> {"ok": true, "containers": [
 *                                     {"name": "...", "state": "running"}]}
 *   {"op": "stop",    "name": n} -> {"ok": true} | {"ok": false, "error": ...}
 *   {"op": "start",   "name": n}
 *   {"op": "restart", "name": n}
 *   {"op": "install", "name": n} install <inbox>/n.aember (an update): stop,
 *                                 install, start; the old version stays
 *   {"op": "rollback", "name": n} back to the version before: stop, swap,
 *                                 start
 *
 * A stop through the socket is a requested stop: no restart_policy restart
 * follows until a start. A container that is bind-mounted the socket's
 * directory can control every container, so only trusted ones get it.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace aember::containerd {

/// What the control socket acts on (containerd's loop; a fake in tests).
class ControlTarget {
 public:
  struct Entry {
    std::string name;
    std::string state;
  };
  virtual ~ControlTarget() = default;
  [[nodiscard]] virtual std::vector<Entry> List() const = 0;
  [[nodiscard]] virtual bool Has(std::string_view name) const = 0;
  virtual bool Stop(std::string_view name) = 0;
  virtual bool Start(std::string_view name) = 0;
  /// Install the package waiting as <inbox>/<name>.aember: stop the
  /// container, install it (the old version kept for a rollback), start it.
  virtual std::expected<void, std::string> Install(std::string_view name) = 0;
  /// Back to the version before the last install: stop, swap, start.
  virtual std::expected<void, std::string> Rollback(std::string_view name) = 0;
};

/// The answer to one request (never throws).
nlohmann::json HandleControlRequest(const nlohmann::json& request,
                                    ControlTarget& target);

/// The answer (JSON text) to one raw request as it came off the socket:
/// bytes that aren't JSON get an error answer. Never throws.
std::string AnswerControlRequest(std::string_view request,
                                 ControlTarget& target);

/// Listening SOCK_SEQPACKET socket at @p path (its directory is created
/// 0700, a stale socket replaced); -errno on failure.
int OpenControlSocket(const std::string& path);

/// Accept one connection on @p listen_fd, read its request, answer it.
/// Returns false if nothing could be accepted.
bool ServeControlConnection(int listen_fd, ControlTarget& target);

}  // namespace aember::containerd
