#include <aember-libs/utils/module/module-parser.h>

namespace aember::utils::module {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ModulesConfigParser::ParseFile(const std::string& path,
                                    config::ConfigError& error) {
  nlohmann::json json;

  if (!LoadJsonFromFile(path, json, error)) { return false; }

  modules_.clear();
  return Parse(json, error);
}

const std::vector<ModuleConfig>& ModulesConfigParser::GetModules() const {
  return modules_;
}

// ---------------------------------------------------------------------------
// Internal parsing
// ---------------------------------------------------------------------------

bool ModulesConfigParser::Parse(const nlohmann::json& json,
                                config::ConfigError& error) {
  if (!json.is_object()) {
    error = config::ConfigError("Root must be a JSON object");
    return false;
  }

  if (!json.contains("modules")) {
    error = config::ConfigError("Missing 'modules'");
    return false;
  }

  const auto& modules = json["modules"];

  if (!modules.is_array()) {
    error = config::ConfigError("'modules' must be an array");
    return false;
  }

  for (const auto& entry : modules) {
    ModuleConfig cfg;

    if (!ParseModule(entry, cfg, error)) { return false; }

    modules_.push_back(std::move(cfg));
  }

  return true;
}

bool ModulesConfigParser::ParseModule(const nlohmann::json& json,
                                      ModuleConfig& cfg,
                                      config::ConfigError& error) {
  // -----------------------------------------------------------------------
  // Format 1: string
  // Example: "kvm"
  // -----------------------------------------------------------------------
  if (json.is_string()) {
    cfg.name = json.get<std::string>();
  }

  // -----------------------------------------------------------------------
  // Format 2: object
  // Example: { "name": "kvm" }
  // -----------------------------------------------------------------------
  else if (json.is_object()) {
    if (!json.contains("name") || !json["name"].is_string()) {
      error = config::ConfigError("Module object missing 'name'");
      return false;
    }

    cfg.name = json["name"].get<std::string>();
  }

  // -----------------------------------------------------------------------
  // Invalid type
  // -----------------------------------------------------------------------
  else {
    error = config::ConfigError("Module entry must be string or object");
    return false;
  }

  // -----------------------------------------------------------------------
  // Validation
  // -----------------------------------------------------------------------
  if (cfg.name.empty()) {
    error = config::ConfigError("Module name cannot be empty");
    return false;
  }

  return true;
}

}  // namespace aember::utils::module
