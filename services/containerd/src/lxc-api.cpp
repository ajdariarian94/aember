/**
 * @file lxc-api.cpp
 * @author Arian Ajdari
 * @brief liblxc, loaded with dlopen on first use.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/containerd/lxc-api.h>

#include <mutex>

#include <dlfcn.h>

namespace aember::containerd {

namespace {

struct Loaded {
  LxcApi api{};
  bool ok = false;
  std::string error;
};

const Loaded& Load() {
  static const Loaded loaded = [] {
    Loaded l;
    void* handle = ::dlopen("liblxc.so.1", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      l.error = ::dlerror();
      return l;
    }
    l.api.container_new = reinterpret_cast<decltype(l.api.container_new)>(
        ::dlsym(handle, "lxc_container_new"));
    l.api.container_put = reinterpret_cast<decltype(l.api.container_put)>(
        ::dlsym(handle, "lxc_container_put"));
    if (l.api.container_new == nullptr || l.api.container_put == nullptr) {
      l.error = "liblxc.so.1 lacks lxc_container_new/put";
      return l;
    }
    l.ok = true;
    return l;  // handle stays open for the process lifetime
  }();
  return loaded;
}

const LxcApi* g_test_api = nullptr;

}  // namespace

const LxcApi* Lxc() {
  if (g_test_api != nullptr) return g_test_api;
  const auto& l = Load();
  return l.ok ? &l.api : nullptr;
}

void SetLxcForTesting(const LxcApi* api) {
  g_test_api = api;
}

std::string LxcLoadError() {
  return Load().error;
}

}  // namespace aember::containerd
