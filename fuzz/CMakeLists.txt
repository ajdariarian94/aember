# aember's fuzz targets: libFuzzer + AddressSanitizer + UndefinedBehavior-
# Sanitizer over the code that reads input (configs, the daemon channel,
# containerd's control socket, DHCP replies, the status file).
#
# Built by `aember fuzz`, not by the main build: clang 18 (the dev image's; the
# SDK's GCC has no libFuzzer) compiles aember's sources against the x64 SDK
# sysroot (its glibc and libstdc++ 15) and the x64 Debug build's vcpkg packages.
# The binaries load the sysroot's libraries (dynamic linker and rpath), like the
# unit tests do.
cmake_minimum_required(VERSION 3.25)
project(
  aember-fuzz
  LANGUAGES CXX
)

set(AEMBER_SYSROOT
    ""
    CACHE PATH
          "x64 SDK target sysroot (core2-64-poky-linux)"
)
set(AEMBER_VCPKG_INSTALLED
    ""
    CACHE PATH
          "vcpkg_installed/x64-poky-linux of the x64 Debug build"
)
if (NOT
    IS_DIRECTORY
    "${AEMBER_SYSROOT}"
    OR NOT
       IS_DIRECTORY
       "${AEMBER_VCPKG_INSTALLED}"
)
  message(
    FATAL_ERROR
      "Set AEMBER_SYSROOT and AEMBER_VCPKG_INSTALLED (aember fuzz does)"
  )
endif ()

set(CMAKE_CXX_STANDARD
    23
)
set(CMAKE_CXX_STANDARD_REQUIRED
    ON
)
set(CMAKE_CXX_EXTENSIONS
    OFF
)
list(
  PREPEND
  CMAKE_PREFIX_PATH
  "${AEMBER_VCPKG_INSTALLED}"
)
set(CMAKE_FIND_ROOT_PATH
    "${AEMBER_SYSROOT};${AEMBER_VCPKG_INSTALLED}"
)

find_package(
  nlohmann_json
  CONFIG
  REQUIRED
)
find_package(
  spdlog
  CONFIG
  REQUIRED
)

get_filename_component(
  AEMBER_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  ABSOLUTE
)

add_compile_options(
  --target=x86_64-poky-linux
  --sysroot=${AEMBER_SYSROOT}
  # libstdc++ 15 enables <expected> only for concepts >= 202002L, which clang 18
  # doesn't claim yet (same as `aember analyze`).
  -D__cpp_concepts=202002L
  -Wno-builtin-macro-redefined
  -g
  -O1
  -fno-omit-frame-pointer
  -fsanitize=address,undefined
  # Undefined behaviour stops the run (libFuzzer saves the input).
  -fno-sanitize-recover=all
)
add_link_options(
  --target=x86_64-poky-linux
  --sysroot=${AEMBER_SYSROOT}
  -fsanitize=address,undefined
  -Wl,--dynamic-linker=${AEMBER_SYSROOT}/lib/ld-linux-x86-64.so.2
  -Wl,-rpath,${AEMBER_SYSROOT}/lib:${AEMBER_SYSROOT}/usr/lib
)

# The code under test, coverage-instrumented for libFuzzer.
add_library(
  aember-fuzzed
  STATIC
  ${AEMBER_ROOT}/libraries/aember-init/src/daemon-status.cpp
  ${AEMBER_ROOT}/libraries/device-health/src/status.cpp
  ${AEMBER_ROOT}/libraries/service-manager/src/dependency-resolver.cpp
  ${AEMBER_ROOT}/libraries/utils/config/src/config-file-parser.cpp
  ${AEMBER_ROOT}/libraries/utils/ipc/src/channel.cpp
  ${AEMBER_ROOT}/libraries/utils/logging/src/logging.cpp
  ${AEMBER_ROOT}/libraries/utils/module/src/module-parser.cpp
  ${AEMBER_ROOT}/libraries/utils/netlink/src/netlink.cpp
  ${AEMBER_ROOT}/libraries/utils/process/src/process-config-parser.cpp
  ${AEMBER_ROOT}/services/containerd/src/container-parser.cpp
  ${AEMBER_ROOT}/services/containerd/src/control.cpp
  ${AEMBER_ROOT}/services/netd/src/dhcp-client.cpp
  ${AEMBER_ROOT}/services/netd/src/network-manager.cpp
)
target_compile_options(
  aember-fuzzed
  PUBLIC -fsanitize=fuzzer-no-link
)
foreach (
  dir
  libraries/aember-init
  libraries/device-health
  libraries/service-manager
  libraries/utils/config
  libraries/utils/ipc
  libraries/utils/logging
  libraries/utils/module
  libraries/utils/netlink
  libraries/utils/process
  services/containerd
  services/netd
)
  target_include_directories(
    aember-fuzzed
    PUBLIC ${AEMBER_ROOT}/${dir}/include
  )
endforeach ()
target_link_libraries(
  aember-fuzzed
  PUBLIC nlohmann_json::nlohmann_json
         spdlog::spdlog
)

set(AEMBER_FUZZ_TARGETS
    fuzz_containers_json
    fuzz_control
    fuzz_dhcp
    fuzz_ipc
    fuzz_modules_json
    fuzz_network_json
    fuzz_services_json
    fuzz_status
)
foreach (
  target
  ${AEMBER_FUZZ_TARGETS}
)
  add_executable(
    ${target}
    targets/${target}.cpp
  )
  target_link_libraries(
    ${target}
    PRIVATE aember-fuzzed
  )
  target_link_options(
    ${target}
    PRIVATE
    -fsanitize=fuzzer
  )
endforeach ()
