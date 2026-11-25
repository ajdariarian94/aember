/**
 * @file fuzz_status.cpp
 * @brief PID 1's status file (/var/log/aember-status.json): the payload
 *        built from what daemons reported (any JSON, plus raw bytes as a
 *        string, valid UTF-8 or not) written by StatusReporter::WriteFile
 *        over the previous content; and /proc/uptime's text through
 *        ParseUptimeSeconds.
 *        Properties: never crashes or throws; the file always holds exactly
 *        one JSON document (aember-stats and aember-monitor read it), equal
 *        to the payload when that was valid UTF-8; the uptime is >= 0.
 */
#include <aember-libs/device-health/status.h>

#include <sstream>

#include "common.h"

namespace {

std::string Read(const std::string& path) {
  std::ifstream f{path, std::ios::binary};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  const auto text = fuzz::Text(data, size);
  if (aember::device_health::ParseUptimeSeconds(text) < 0) {
    fuzz::Fail("negative uptime");
  }

  static const auto path = fuzz::TempFile("aember-status.json");
  static const aember::device_health::StatusReporter status;

  const auto parsed = nlohmann::json::parse(text, nullptr, false);
  nlohmann::json payload = {{"status", "online"},
                            {"network", {{"interface", std::string{text}}}}};
  if (!parsed.is_discarded()) payload["services"] = parsed;

  status.WriteFile(payload, path);
  const auto written = nlohmann::json::parse(Read(path), nullptr, false);
  if (written.is_discarded()) fuzz::Fail("status file isn't JSON");
  if (!parsed.is_discarded() && written["services"] != parsed) {
    fuzz::Fail("status file lost the payload");
  }
  return 0;
}
