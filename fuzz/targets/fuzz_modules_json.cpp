/**
 * @file fuzz_modules_json.cpp
 * @brief modules.json as PID 1 reads it (ModulesConfigParser).
 *        Properties: never crashes or throws; no module has an empty name.
 */
#include <aember-libs/utils/module/module-parser.h>

#include "common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  static const auto path = fuzz::TempFile("modules.json");
  fuzz::Write(path, fuzz::Text(data, size));

  aember::utils::module::ModulesConfigParser parser;
  aember::utils::config::ConfigError error;
  if (!parser.ParseFile(path, error)) return 0;
  for (const auto& m : parser.GetModules()) {
    if (m.name.empty()) fuzz::Fail("module with an empty name");
  }
  return 0;
}
