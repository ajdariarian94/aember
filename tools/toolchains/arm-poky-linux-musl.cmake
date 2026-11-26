# arm-poky-linux with musl (aember-resources' opt-in arm-poky-linux-musl SDK): the
# arm-poky-linux toolchain with the musl sysroot and compiler prefix.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ---------------------------------------------------------------------------
# Poky Paths — SDK and LXC from aember-resources, fetched by
# `aember fetch-resources` into build/aember-resources/<arch>/
# ---------------------------------------------------------------------------
get_filename_component(AEMBER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AEMBER_RESOURCES_DIR "${AEMBER_ROOT}/build/aember-resources/arm-poky-linux-musl")
set(POKY_SYSROOT  "${AEMBER_RESOURCES_DIR}/sdk/sysroots/armv7at2hf-neon-poky-linux-musleabi")
set(POKY_SDK_ROOT "${AEMBER_RESOURCES_DIR}/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers — Poky SDK cross GCC (32-bit ARMv7-A, hard-float, NEON)
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-musleabi/arm-poky-linux-musleabi-gcc"
)
set(CMAKE_CXX_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-musleabi/arm-poky-linux-musleabi-g++"
)

# Find the SDK's arm binutils before the host x86 ones.
list(APPEND CMAKE_PROGRAM_PATH
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-musleabi"
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
# Same tune as the SDK (Poky armv7athf-neon, see TUNE_CCARGS), so it runs on
# Cortex-A7, A9 and A15 boards:
#   -march=armv7-a        baseline ARMv7-A instruction set
#   -mthumb               Thumb-2 code (smaller), like the SDK's libraries
#   -mfpu=neon            NEON SIMD + VFPv3
#   -mfloat-abi=hard      hard-float ABI; must match the sysroot (musleabi hf)
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-march=armv7-a -mthumb -mfpu=neon -mfloat-abi=hard -fstack-protector-strong"
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

# musl's default thread stack is 128 KB (glibc: 8 MB, from ulimit -s). musl
# takes the default from PT_GNU_STACK instead: give every thread (hub,
# reaper, watchdog, ...) 1 MB.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-z,stack-size=1048576")

set(CMAKE_C_FLAGS_INIT          "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip using the arm cross-strip
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  find_program(STRIP_TOOL NAMES arm-poky-linux-musleabi-strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-musleabi"
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

# Prepend arm cross-tools so g++ finds arm-poky-linux-musleabi-as
# and arm-poky-linux-musleabi-ld rather than the host x86 binaries.
set(ENV{PATH} "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-musleabi:$ENV{PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}         "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}   "1")
endif()
# Coverage / sanitizer test builds (AEMBER_INSTRUMENT)
include("${CMAKE_CURRENT_LIST_DIR}/instrument.cmake")
