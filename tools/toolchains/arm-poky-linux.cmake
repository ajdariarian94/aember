set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ---------------------------------------------------------------------------
# Poky Paths — arm32
# ---------------------------------------------------------------------------
set(POKY_SYSROOT  "/aember_ws/aember/yocto/build/arm-poky-linux/sdk/sysroots/cortexa9hf-neon-poky-linux-gnueabi")
set(POKY_SDK_ROOT "/aember_ws/aember/yocto/build/arm-poky-linux/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi/arm-poky-linux-gnueabi-gcc"
)
set(CMAKE_CXX_COMPILER
  "/aember_ws/aember/yocto/build/arm-poky-linux/gcc-reflection/install/bin/arm-poky-linux-gnueabi-g++"
)

# Add SDK cross-tools to program path so the reflection g++ finds the correct
# arm assembler and linker instead of the host x86 ones.
list(APPEND CMAKE_PROGRAM_PATH
  "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi"
)

set(ENV{PATH} "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi:$ENV{PATH}")

# ---------------------------------------------------------------------------
# C++ Standard — C++26 with P2996 reflection
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Base flags — Cortex-A9 with NEON, hard-float ABI
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fstack-protector-strong"
)
set(REFLECTION_FLAGS "-freflection")

# ---------------------------------------------------------------------------
# Release optimizations
# ---------------------------------------------------------------------------
set(RELEASE_FLAGS
  "-O3 -flto=auto -ffunction-sections -fdata-sections -fvisibility=hidden"
)
set(RELEASE_LINK_FLAGS
  "-flto=auto -Wl,--gc-sections -Wl,--as-needed -Wl,--strip-all -static-libgcc"
)

set(CMAKE_C_FLAGS_INIT          "${POKY_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS} ${REFLECTION_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  find_program(STRIP_TOOL NAMES arm-poky-linux-gnueabi-strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/arm-poky-linux-gnueabi"
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
set(VIRT_ROOT            "/aember_ws/aember/yocto/build/arm-poky-linux/virtualization")
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/pkgconfig")
set(SYSROOT_PKG_CONFIG   "${POKY_SYSROOT}/usr/lib/pkgconfig")

list(APPEND CMAKE_FIND_ROOT_PATH "${VIRT_ROOT}")

set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:${SYSROOT_PKG_CONFIG}:$ENV{PKG_CONFIG_PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}         "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}   "1")
endif()