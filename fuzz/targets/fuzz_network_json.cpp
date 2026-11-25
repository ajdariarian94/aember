/**
 * @file fuzz_network_json.cpp
 * @brief network.json as aember-netd reads it (the NetworkManager
 *        constructor). A bad file may throw (netd logs it and exits, PID 1
 *        restarts it); anything else - a crash, undefined behaviour, an
 *        exception that isn't std::exception - is a bug.
 */
#include <aember-services/netd/network-manager.h>

#include <exception>

#include "common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  const auto config =
      nlohmann::json::parse(fuzz::Text(data, size), nullptr, false);
  if (config.is_discarded()) return 0;  // netd: "cannot read"
  try {
    aember::netd::NetworkManager network{config};
  } catch (const std::exception&) {
    // netd's main catches these: "network bring-up failed"
  }
  return 0;
}
