set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ---------------------------------------------------------------------------
# Poky Paths — SDK and LXC from aember-resources, fetched by
# `aember fetch-resources` into build/aember-resources/<arch>/
# ---------------------------------------------------------------------------
get_filename_component(AEMBER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AEMBER_RESOURCES_DIR "${AEMBER_ROOT}/build/aember-resources/arm-poky-linux")
set(POKY_SYSROOT  "${AEMBER_RESOURCES_DIR}/sdk/sysroots/armv7at2hf-neon-poky-linux-gnueabi")
set(POKY_SDK_ROOT "${AEMBER_RESOURCES_DIR}/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers — Poky SDK cross GCC (32-bit ARMv7-A, hard-float, NEON)
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi/arm-poky-linux-gnueabi-gcc"
)
set(CMAKE_CXX_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi/arm-poky-linux-gnueabi-g++"
)

# Find the SDK's arm binutils before the host x86 ones.
list(APPEND CMAKE_PROGRAM_PATH
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi"
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
#   -mfloat-abi=hard      hard-float ABI; must match the sysroot (gnueabi hf)
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-march=armv7-a -mthumb -mfpu=neon -mfloat-abi=hard -fstack-protector-strong"
)

# ---------------------------------------------------------------------------
# Release optimizations (same as x64, architecture-agnostic)
# ---------------------------------------------------------------------------
set(RELEASE_FLAGS
  "-O3 -flto=auto -ffunction-sections -fdata-sections -fvisibility=hidden"
)

set(RELEASE_LINK_FLAGS
  "-flto=auto -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all -static-libgcc"
)

set(CMAKE_C_FLAGS_INIT          "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip using the arm cross-strip
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  find_program(STRIP_TOOL NAMES arm-poky-linux-gnueabi-strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi"
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

# Prepend arm cross-tools so g++ finds arm-poky-linux-gnueabi-as
# and arm-poky-linux-gnueabi-ld rather than the host x86 binaries.
set(ENV{PATH} "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi:$ENV{PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}         "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}   "1")
endif()