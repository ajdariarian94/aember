/**
 * @file lxc-api.h
 * @author Arian Ajdari
 * @brief liblxc, loaded with dlopen on first use.
 *
 *        liblxc (4.8 MB) is not linked into the `aember` binary: only the
 *        `aember containerd` role loads it, so PID 1 never maps it. Of its
 *        API only lxc_container_new/put are plain symbols; everything else
 *        goes through the function pointers in struct lxc_container.
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <string>

struct lxc_container;

namespace aember::container_manager {

struct LxcApi {
  lxc_container* (*container_new)(const char* name, const char* configpath);
  int (*container_put)(lxc_container* c);
};

/// liblxc's API, loaded once (thread-safe); nullptr if liblxc.so.1 can't be
/// loaded (then LxcLoadError() says why).
const LxcApi* Lxc();

/// Why Lxc() returned nullptr ("" if it didn't).
std::string LxcLoadError();

}  // namespace aember::container_manager
