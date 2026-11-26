# x64 with musl (aember-resources' opt-in x64-poky-linux-musl SDK): the
# x64-poky-linux toolchain with the musl sysroot and compiler prefix.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ---------------------------------------------------------------------------
# Poky Paths — SDK and LXC from aember-resources, fetched by
# `aember fetch-resources` into build/aember-resources/<arch>/
# ---------------------------------------------------------------------------
get_filename_component(AEMBER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AEMBER_RESOURCES_DIR "${AEMBER_ROOT}/build/aember-resources/x64-poky-linux-musl")
set(POKY_SYSROOT  "${AEMBER_RESOURCES_DIR}/sdk/sysroots/core2-64-poky-linux-musl")
set(POKY_SDK_ROOT "${AEMBER_RESOURCES_DIR}/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER   "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux-musl/x86_64-poky-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux-musl/x86_64-poky-linux-musl-g++")

# ---------------------------------------------------------------------------
# C++ Standard — C++23 (Poky SDK GCC 15)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Base flags (architecture + hardening)
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-m64 -march=core2 -mtune=core2 -msse3 -mfpmath=sse -fstack-protector-strong"
)

# ---------------------------------------------------------------------------
# Release optimizations
#
# -Os                  Optimize for size: 18-24 % smaller programs than -O3
#                      with the same boot time and idle CPU, measured in
#                      doc/performance.md (they mostly wait).
# -flto                Link-time optimization — biggest size/speed win.
#                      Must appear on both compiler and linker flags.
# -ffunction-sections  Each function in its own section → --gc-sections can
#                      remove unreachable ones at link time.
# -fdata-sections      Same for variables.
# -fvisibility=hidden  All symbols hidden by default; only explicitly exported
#                      symbols are visible. Reduces binary size and prevents
#                      accidental ABI exposure.
#
# NOT added:
# -fno-exceptions      nlohmann/json and std::expected use exceptions.
# -fno-rtti            std::any / dynamic_cast used indirectly by some deps.
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

# ---------------------------------------------------------------------------
# Release linker flags
#
# -flto                Must match compiler -flto.
# --gc-sections        Remove sections with no incoming references (dead code).
#                      Only effective when paired with -ffunction-sections.
# --as-needed          Only link libraries actually referenced.
# --strip-all          Strip all symbols at link time (saves a separate strip
#                      invocation; remove if you need post-link debug info).
# ---------------------------------------------------------------------------
set(RELEASE_LINK_FLAGS
  "-flto=${AEMBER_LTO_JOBS} -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all"
)

# musl's default thread stack is 128 KB (glibc: 8 MB, from ulimit -s). musl
# takes the default from PT_GNU_STACK instead: give every thread (hub,
# reaper, watchdog, ...) 1 MB, like glibc's typical working set needs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-z,stack-size=1048576")

set(CMAKE_C_FLAGS_INIT          "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip (belt-and-suspenders with --strip-all above,
# and also strips .comment / .note sections that --strip-all misses).
# Applied only in Release — set via cmake --config Release or
# -DCMAKE_BUILD_TYPE=Release.
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  # The SDK's cross strip. NO_CMAKE_FIND_ROOT_PATH: don't pick the target
  # sysroot's /usr/bin/strip, which is a dangling link there.
  find_program(STRIP_TOOL NAMES x86_64-poky-linux-musl-strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux-musl"
    NO_CMAKE_FIND_ROOT_PATH
  )

  if(STRIP_TOOL)
    # Applied to every executable target via a global post-build hook.
    # Individual targets can opt out by unsetting STRIP_TARGET property.
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

# BOTH so spdlog can find the fmt vcpkg package.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  list(APPEND CMAKE_FIND_ROOT_PATH "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

# ---------------------------------------------------------------------------
# pkg-config search paths
# ---------------------------------------------------------------------------
set(VIRT_ROOT           "${AEMBER_RESOURCES_DIR}/virtualization")
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/pkgconfig")
set(SYSROOT_PKG_CONFIG   "${POKY_SYSROOT}/usr/lib/pkgconfig")

list(APPEND CMAKE_FIND_ROOT_PATH "${VIRT_ROOT}")

set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:${SYSROOT_PKG_CONFIG}:$ENV{PKG_CONFIG_PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}          "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS}  "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}    "1")
endif()
# Coverage / sanitizer test builds (AEMBER_INSTRUMENT)
include("${CMAKE_CURRENT_LIST_DIR}/instrument.cmake")
