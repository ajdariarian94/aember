# aember-containerd: LXC containers (containers.json) and their bridges.

find_package(
  aember-libs
  REQUIRED
  COMPONENTS child-supervisor
             mount-manager
             utils
)

# LXC from aember-resources; VIRT_ROOT is set by the toolchain file
# (build/aember-resources/<arch>/virtualization). liblxc is not linked:
# src/lxc-api.cpp loads it with dlopen, so only its headers are needed here
# (`aember app pack` packs liblxc together with this service).
if (NOT
    VIRT_ROOT
)
  message(FATAL_ERROR "VIRT_ROOT not set — use tools/toolchains/<arch>.cmake")
endif ()
set(LXC_INCLUDE_DIR
    "${VIRT_ROOT}/include"
)
if (NOT
    EXISTS
    ${LXC_INCLUDE_DIR}/lxc/lxccontainer.h
)
  message(FATAL_ERROR "lxccontainer.h not found at ${LXC_INCLUDE_DIR}")
endif ()

aember_service(
  aember-containerd
  MAIN
  src/main.cpp
  SOURCES
  src/container-manager.cpp
  src/container-parser.cpp
  src/container-state.cpp
  src/containerd.cpp
  src/control.cpp
  src/lxc-api.cpp
  src/start-graph.cpp
  LIBRARIES
  aember-libs::child-supervisor
  aember-libs::mount-manager
  aember-libs::utils::config
  aember-libs::utils::ipc
  aember-libs::utils::logging
  aember-libs::utils::netlink
  aember-libs::utils::package
  nlohmann_json::nlohmann_json
  ${CMAKE_DL_LIBS}
  PRIVATE_INCLUDES
  ${LXC_INCLUDE_DIR}
)

if (BUILD_TESTS)
  add_subdirectory(tests)
endif ()
