set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Poky Paths
set(POKY_SYSROOT "/aember_ws/aember/yocto/build/x64-poky-linux/sdk/sysroots/core2-64-poky-linux")
set(POKY_SDK_ROOT "/aember_ws/aember/yocto/build/x64-poky-linux/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# Compilers
set(CMAKE_C_COMPILER "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux/x86_64-poky-linux-gcc")
set(CMAKE_CXX_COMPILER "/aember_ws/aember/yocto/build/x64-poky-linux/gcc-reflection/install/bin/g++")

# ---------------------------------------------------------------------------
# C++ Standard — C++26 with experimental P2996 reflection
#
# -std=c++26         : enables C++26 language features (expected chaining,
#                      ranges::to, move_only_function, etc.)
# -freflection       : enables P2996 static reflection (experimental, GCC
#                      reflection branch only — not upstream yet)
# -freflection-ts    : enables reflection TS extensions on some builds;
#                      remove if your gcc-reflection branch doesn't need it
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler Flags
set(POKY_FLAGS "-m64 -march=core2 -mtune=core2 -msse3 -mfpmath=sse -fstack-protector-strong")
set(REFLECTION_FLAGS "-freflection")

set(CMAKE_C_FLAGS_INIT   "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${POKY_FLAGS} ${REFLECTION_FLAGS}")

# ---------------------------------------------------------------------------
# vcpkg / find_* root path modes
# ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# BOTH so spdlog can find the fmt vcpkg package
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

# ---------------------------------------------------------------------------
# pkg-config search paths
# ---------------------------------------------------------------------------
set(VIRT_ROOT "/aember_ws/aember/yocto/build/x64-poky-linux/virtualization")
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/pkgconfig")
set(SYSROOT_PKG_CONFIG "${POKY_SYSROOT}/usr/lib/pkgconfig")

list(APPEND CMAKE_FIND_ROOT_PATH "${VIRT_ROOT}")

set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:${SYSROOT_PKG_CONFIG}:$ENV{PKG_CONFIG_PATH}")

if("${TRIPLET}" MATCHES "poky")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")
    set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
    set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS} "1")
endif()