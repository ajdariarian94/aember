/**
 * @file restart-policy.h
 * @author Arian Ajdari
 * @brief Enum class for RestartPolicy
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */
#include <vector>

#include <aember-libs/utils/config/iconfig-file-parser.h>
#include <aember-libs/utils/service/service-config.h>

namespace aember::utils::service {

class ServicesConfigParser : public config::IConfigFileParser {
 public:
  bool ParseFile(const std::string& path, config::ConfigError& error) override;

  const std::vector<ServiceConfig>& GetServices() const;

 private:
  bool Parse(const nlohmann::json& json, config::ConfigError& error);

  bool ParseService(const nlohmann::json& json, ServiceConfig& svc,
                    config::ConfigError& error);

  RestartPolicy ParseRestartPolicy(const std::string& policy);

 private:
  std::vector<ServiceConfig> services_;
};

}  // namespace aember::utils::service
