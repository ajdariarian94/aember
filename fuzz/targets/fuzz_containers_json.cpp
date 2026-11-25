/**
 * @file fuzz_containers_json.cpp
 * @brief containers.json as aember-containerd reads it: the containers
 *        (ContainersConfigParser) and the bridges (ParseBridges).
 *        Properties: never crashes or throws; every bridge has a name.
 */
#include <aember-services/containerd/container-parser.h>

#include "common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  static const auto path = fuzz::TempFile("containers.json");
  fuzz::Write(path, fuzz::Text(data, size));

  aember::containerd::ContainersConfigParser parser;
  aember::utils::config::ConfigError error;
  std::ignore = parser.ParseFile(path, error);

  const auto config =
      nlohmann::json::parse(fuzz::Text(data, size), nullptr, false);
  std::vector<std::string> errors;
  for (const auto& bridge : aember::containerd::ParseBridges(config, errors)) {
    if (bridge.name.empty()) fuzz::Fail("bridge without a name");
  }
  return 0;
}
