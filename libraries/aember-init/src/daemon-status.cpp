/**
 * @file daemon-status.cpp
 * @author Arian Ajdari
 * @brief ApplyDaemonMessage (see daemon-status.h).
 * @version 0.1
 * @date 2026-11-25
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/aember-init/daemon-status.h>
#include <aember-libs/utils/ipc/channel.h>

#include <ctime>

namespace aember::aember_init {

namespace {

bool Online(const nlohmann::json& j) {
  if (!j.is_object()) return false;
  const auto it = j.find("online");
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::string Text(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>()
                                               : std::string{};
}

}  // namespace

DaemonUpdate ApplyDaemonMessage(const nlohmann::json& message,
                                DaemonStatus& status) {
  DaemonUpdate update;
  const auto type = aember::utils::ipc::TypeOf(message);  // "" if no object
  if (type == "ready") {
    update.kind = DaemonUpdate::Kind::kReady;
    return update;
  }
  if (type != "event") return update;
  const auto data = message.find("data");
  if (data == message.end() || !data->is_object()) return update;

  const auto event = Text(message, "name");
  if (event == "network") {
    const bool was_online = Online(status.network);
    status.network = *data;
    update.kind = DaemonUpdate::Kind::kNetwork;
    update.went_offline = was_online && !Online(status.network);
  } else if (event == "container") {
    update.container = Text(*data, "name");
    update.state = Text(*data, "state");
    if (update.container.empty()) return update;
    auto& state = status.containers[update.container];
    if (state != update.state) status.since_ms[update.container] = BootTimeMs();
    state = update.state;
    update.kind = DaemonUpdate::Kind::kContainer;
  }
  return update;
}

long long BootTimeMs() {
  timespec ts{};
  if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
  return (static_cast<long long>(ts.tv_sec) * 1000) + (ts.tv_nsec / 1000000);
}

}  // namespace aember::aember_init
