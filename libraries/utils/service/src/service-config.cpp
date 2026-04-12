/**
 * @file service-config.cpp
 * @author Arian Ajdari
 * @brief Library implementation for service config
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/service/service-config.h>

namespace aember::utils::service {

ServiceConfig::ServiceConfig(const std::string& n, const std::string& cmd)
      : name(n), command(cmd) {}

}  // namespace aember::utils::service
