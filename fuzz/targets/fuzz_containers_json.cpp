/**
 * @file fuzz_containers_json.cpp
 * @brief containers.json as aember-containerd reads it (the containers and
 *        their dependencies: LoadBoardContainers, then the StartGraph; the
 *        bridges: ParseBridges), and the same bytes as a container's
 *        config.json (ParseContainerConfig).
 *        Properties: never crashes or throws; every bridge has a name; the
 *        graph starts only listed entries, each after its dependencies.
 */
#include <aember-services/containerd/container-parser.h>
#include <aember-services/containerd/start-graph.h>

#include <set>

#include "common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  static const auto path = fuzz::TempFile("containers.json");
  fuzz::Write(path, fuzz::Text(data, size));

  const auto config =
      nlohmann::json::parse(fuzz::Text(data, size), nullptr, false);
  std::vector<std::string> errors;
  for (const auto& bridge : aember::containerd::ParseBridges(config, errors)) {
    if (bridge.name.empty()) fuzz::Fail("bridge without a name");
  }
  std::ignore = aember::containerd::ParseContainerConfig(config);

  const auto board = aember::containerd::LoadBoardContainers(path);
  if (!board) return 0;
  aember::containerd::StartGraph graph{board->containers};
  for (const auto& name : graph.Names()) graph.Want(name);
  std::set<std::string> running;
  for (auto ready = graph.TakeReady(); !ready.empty();
       ready = graph.TakeReady()) {
    for (const auto& name : ready) {
      for (const auto& node : board->containers) {
        if (node.name != name) continue;
        for (const auto& dep : node.dependencies) {
          if (!running.contains(dep)) fuzz::Fail("started before a dependency");
        }
        break;
      }
      running.insert(name);
      graph.SetRunning(name, true);
    }
  }
  if (running.size() != graph.Names().size())
    fuzz::Fail("a good entry never started");
  return 0;
}
