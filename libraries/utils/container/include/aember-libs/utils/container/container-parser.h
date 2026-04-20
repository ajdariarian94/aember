/**
 * @file container-parser.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerPec
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <vector>

#include <aember-libs/utils/container/container-spec.h>
#include <aember-libs/utils/config/iconfig-file-parser.h>

namespace aember::utils::container {

class ContainersConfigParser
    : public config::IConfigFileParser {
public:
  bool ParseFile(const std::string& path,
                 config::ConfigError& error) override;

  const std::vector<ContainerSpec>& GetContainers() const;

private:
  bool Parse(const nlohmann::json& json,
             config::ConfigError& error);

private:
  std::vector<ContainerSpec> containers_;
};

}
