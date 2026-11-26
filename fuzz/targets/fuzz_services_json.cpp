/**
 * @file fuzz_services_json.cpp
 * @brief services.json as PID 1 reads it (ProcessConfigParser, with a
 *        folder for the services a, b, c, aember-netd and aember-containerd),
 *        then the start order from the dependencies (DependencyResolver);
 *        and the same bytes as one service's config.json.
 *        Properties: never crashes or throws; every name in the start order
 *        is unique, comes after all of its dependencies, and has none that
 *        is unknown.
 */
#include <aember-libs/service-manager/dependency-resolver.h>
#include <aember-libs/utils/process/process-config-parser.h>

#include <set>

#include "common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  static const auto path = fuzz::TempFile("services.json");
  static const auto folders = [] {
    const auto dir = fuzz::TempFile("services");
    for (const char* name :
         {"a", "b", "c", "aember-netd", "aember-containerd"}) {
      std::filesystem::create_directories(dir + "/" + name);
      fuzz::Write(dir + "/" + name + "/config.json",
                  std::string{R"({"executable": "/usr/bin/)"} + name + "\"}");
    }
    return dir;
  }();
  fuzz::Write(path, fuzz::Text(data, size));

  {
    const auto config =
        nlohmann::json::parse(fuzz::Text(data, size), nullptr, false);
    aember::utils::process::ProcessConfig cfg;
    aember::utils::config::ConfigError error;
    std::ignore =
        aember::utils::process::ProcessConfigParser::ParseServiceConfig(
            config, cfg, error);
  }

  aember::utils::process::ProcessConfigParser parser;
  parser.SetServicesDir(folders);
  aember::utils::config::ConfigError error;
  if (!parser.ParseFile(path, error)) return 0;

  aember::service_manager::DependencyResolver::DependencyGraph graph;
  for (const auto& p : parser.GetProcesses()) graph[p.name] = p.dependencies;
  const aember::service_manager::DependencyResolver resolver{
      [](const std::string&) { return false; }};
  const auto order = resolver.ResolveStartOrder(graph);

  std::set<std::string> started;
  for (const auto& name : order) {
    const auto it = graph.find(name);
    if (it == graph.end()) fuzz::Fail("start order has an unknown name");
    for (const auto& dep : it->second) {
      if (!started.contains(dep)) fuzz::Fail("started before a dependency");
    }
    if (!started.insert(name).second) fuzz::Fail("started twice");
  }
  return 0;
}
