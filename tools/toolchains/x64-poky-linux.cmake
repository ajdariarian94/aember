set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ---------------------------------------------------------------------------
# Poky Paths
# ---------------------------------------------------------------------------
set(POKY_SYSROOT  "/aember_ws/aember/yocto/build/x64-poky-linux/sdk/sysroots/core2-64-poky-linux")
set(POKY_SDK_ROOT "/aember_ws/aember/yocto/build/x64-poky-linux/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER   "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux/x86_64-poky-linux-gcc")
set(CMAKE_CXX_COMPILER "/aember_ws/aember/yocto/build/x64-poky-linux/gcc-reflection/install/bin/g++")

# ---------------------------------------------------------------------------
# C++ Standard — C++26 with P2996 reflection
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Base flags (architecture + hardening)
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-m64 -march=core2 -mtune=core2 -msse3 -mfpmath=sse -fstack-protector-strong"
)
set(REFLECTION_FLAGS "-freflection")

# ---------------------------------------------------------------------------
# Release optimizations
#
# -O3                  Full optimization
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
cmake_host_system_information(RESULT CPU_COUNT QUERY NUMBER_OF_LOGICAL_CORES)
set(RELEASE_FLAGS
  "-O3 -flto=${CPU_COUNT} -ffunction-sections -fdata-sections -fvisibility=hidden"
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
  "-flto -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all"
)

set(CMAKE_C_FLAGS_INIT          "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS} ${REFLECTION_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip (belt-and-suspenders with --strip-all above,
# and also strips .comment / .note sections that --strip-all misses).
# Applied only in Release — set via cmake --config Release or
# -DCMAKE_BUILD_TYPE=Release.
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  find_program(STRIP_TOOL NAMES strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux"
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
set(VIRT_ROOT           "/aember_ws/aember/yocto/build/x64-poky-linux/virtualization")
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/pkgconfig")
set(SYSROOT_PKG_CONFIG   "${POKY_SYSROOT}/usr/lib/pkgconfig")

list(APPEND CMAKE_FIND_ROOT_PATH "${VIRT_ROOT}")

set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:${SYSROOT_PKG_CONFIG}:$ENV{PKG_CONFIG_PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}          "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS}  "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}    "1")
endif()