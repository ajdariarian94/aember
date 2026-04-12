/**
 * @file service-type.h
 * @author Arian Ajdari
 * @brief Enum class for ServiceState
 * @version 0.1
 * @date 2026-04-12
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

namespace aember::utils::service {

/**
 * @brief Type of service: process or container.
 */
enum class ServiceType { PROCESS, CONTAINER };

}  // namespace aember::utils::service
