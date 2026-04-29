set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Poky Paths
set(POKY_SYSROOT "/opt/poky/5.2.99+snapshot/sysroots/core2-64-poky-linux")
set(POKY_SDK_ROOT "/opt/poky/5.2.99+snapshot/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# Compilers
set(CMAKE_C_COMPILER "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux/x86_64-poky-linux-gcc")
set(CMAKE_CXX_COMPILER "${POKY_SDK_ROOT}/usr/bin/x86_64-poky-linux/x86_64-poky-linux-g++")

# Compiler Flags
set(POKY_FLAGS "-m64 -march=core2 -mtune=core2 -msse3 -mfpmath=sse -fstack-protector-strong")
set(CMAKE_C_FLAGS_INIT "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${POKY_FLAGS}")

# --- THE FIX FOR SPDLOG/FMT ---
# Allow CMake to look inside the vcpkg 'installed' directory
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# CRITICAL: Set this to BOTH so spdlog can find the fmt vcpkg package
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Add the vcpkg installed directory to the search path
if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
# ------------------------------

# =====================================
# pkg-config search paths
# =====================================

# 1. Path to your custom-built LXC/Virtualization libraries
# We use REALPATH to resolve those "../.." into a clean, absolute path
get_filename_component(VIRT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../build/${TRIPLET}/virtualization" REALPATH)

# 2. Construct the path to the .pc files. 
# Check your folder: is it 'libs/pkgconfig' or just 'pkgconfig'? 
# Based on your .pc file, it looks like 'libs/pkgconfig'.
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/libs/pkgconfig")

# 3. Update the Environment Variable
# We prepend your custom path to the existing PKG_CONFIG_PATH
set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")

# 4. CROSS-COMPILE PROTECTION
# If we are using the Poky triplet, we MUST tell pkg-config to allow absolute host paths
# and not to accidentally prefix them with the Poky Sysroot.
if("${TRIPLET}" MATCHES "poky")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")
    set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
    set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS} "1")
endif()