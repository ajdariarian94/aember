/**
 * @file daemon-status.h
 * @author Arian Ajdari
 * @brief What PID 1 keeps from its daemons' messages (ipc channel, fd 3):
 *        readiness, netd's network report and containerd's container
 *        states, which go into the status file.
 *
 *        Kept apart from AemberInit so it can be tested and fuzzed alone: a
 *        daemon with a bug may send anything, and PID 1 must never die of
 *        it.
 *
 * @version 0.1
 * @date 2026-11-25
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace aember::aember_init {

struct DaemonStatus {
  /// netd's last "network" event data (an object); null until it reports.
  nlohmann::json network;
  /// containerd's last reported state per container.
  std::map<std::string, std::string> containers;
};

struct DaemonUpdate {
  enum class Kind {
    kIgnored,    ///< not a message PID 1 acts on
    kReady,      ///< {"type": "ready"}
    kNetwork,    ///< network report stored
    kContainer,  ///< container state stored (container, state)
  };
  Kind kind = Kind::kIgnored;
  bool went_offline = false;  ///< kNetwork: online before, not any more
  std::string container;
  std::string state;
};

/// Apply one daemon message to @p status. Never throws.
DaemonUpdate ApplyDaemonMessage(const nlohmann::json& message,
                                DaemonStatus& status);

}  // namespace aember::aember_init
