set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---------------------------------------------------------------------------
# Poky Paths — SDK and LXC from aember-resources, fetched by
# `aember fetch-resources` into build/aember-resources/<arch>/
# ---------------------------------------------------------------------------
get_filename_component(AEMBER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AEMBER_RESOURCES_DIR "${AEMBER_ROOT}/build/aember-resources/aarch64-poky-linux")
set(POKY_SYSROOT  "${AEMBER_RESOURCES_DIR}/sdk/sysroots/cortexa53-poky-linux")
set(POKY_SDK_ROOT "${AEMBER_RESOURCES_DIR}/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers — Poky SDK cross GCC (generic ARMv8-A: Raspberry Pi 3/4, i.MX8, ...)
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux/aarch64-poky-linux-gcc"
)
set(CMAKE_CXX_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux/aarch64-poky-linux-g++"
)

# Find the SDK's aarch64 binutils before the host x86 ones.
list(APPEND CMAKE_PROGRAM_PATH
  "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux"
)

# ---------------------------------------------------------------------------
# C++ Standard — C++23 (Poky SDK GCC 15)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Base flags
#
# Generic ARMv8.0-A, the SDK's cortexa53 tune (aember-resources v0.1.8), so
# one build runs on every 64-bit Arm board (Pi 3/4/Zero 2 W, i.MX8, Rockchip):
#   -march=armv8-a       baseline ARMv8-A instruction set
#   -mtune=cortex-a53    schedule for the A53; the code runs on A72 etc. too
#   -mfpu is not needed on aarch64 — FPU is always present
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-march=armv8-a -mtune=cortex-a53 -fstack-protector-strong"
)

# ---------------------------------------------------------------------------
# Release optimizations (same as x64, architecture-agnostic)
# ---------------------------------------------------------------------------
# LTO jobs: AEMBER_BUILD_JOBS (exported by the aember CLI), else 2/3 of the
# logical cores. -flto=auto would use every core, which crashes WSL.
if(DEFINED ENV{AEMBER_BUILD_JOBS})
  set(AEMBER_LTO_JOBS "$ENV{AEMBER_BUILD_JOBS}")
else()
  cmake_host_system_information(RESULT _aember_cores QUERY NUMBER_OF_LOGICAL_CORES)
  math(EXPR AEMBER_LTO_JOBS "${_aember_cores} * 2 / 3")
  if(AEMBER_LTO_JOBS LESS 1)
    set(AEMBER_LTO_JOBS 1)
  endif()
endif()
set(RELEASE_FLAGS
  "-Os -flto=${AEMBER_LTO_JOBS} -ffunction-sections -fdata-sections -fvisibility=hidden"
)

set(RELEASE_LINK_FLAGS
  "-flto=${AEMBER_LTO_JOBS} -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all -static-libgcc"
)

set(CMAKE_C_FLAGS_INIT          "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip using aarch64 cross-strip
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  find_program(STRIP_TOOL NAMES aarch64-poky-linux-strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux"
    NO_CMAKE_FIND_ROOT_PATH
  )

  if(STRIP_TOOL)
    set(AEMBER_STRIP_COMMAND
      "${STRIP_TOOL}"
      "--strip-all"
      "--remove-section=.comment"
      "--remove-section=.note"
      "--remove-section=.note.ABI-tag"
      "--remove-section=.note.gnu.build-id"
      "--remove-section=.gnu.version"
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# vcpkg / find_* root path modes
# ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  list(APPEND CMAKE_FIND_ROOT_PATH "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

# ---------------------------------------------------------------------------
# pkg-config search paths
# ---------------------------------------------------------------------------
set(VIRT_ROOT            "${AEMBER_RESOURCES_DIR}/virtualization")
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/pkgconfig")
set(SYSROOT_PKG_CONFIG   "${POKY_SYSROOT}/usr/lib/pkgconfig")

list(APPEND CMAKE_FIND_ROOT_PATH "${VIRT_ROOT}")

set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:${SYSROOT_PKG_CONFIG}:$ENV{PKG_CONFIG_PATH}")

# Prepend aarch64 cross-tools so g++ finds aarch64-poky-linux-as
# and aarch64-poky-linux-ld rather than the host x86 binaries.
set(ENV{PATH} "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux:$ENV{PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}         "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}   "1")
endif()
# Coverage / sanitizer test builds (AEMBER_INSTRUMENT)
include("${CMAKE_CURRENT_LIST_DIR}/instrument.cmake")
