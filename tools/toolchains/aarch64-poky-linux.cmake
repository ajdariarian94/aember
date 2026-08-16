set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---------------------------------------------------------------------------
# Poky Paths — aarch64 (Raspberry Pi 4)
# ---------------------------------------------------------------------------
set(POKY_SYSROOT  "/aember_ws/aember/yocto/build/aarch64-poky-linux/sdk/sysroots/cortexa57-poky-linux")
set(POKY_SDK_ROOT "/aember_ws/aember/yocto/build/aarch64-poky-linux/sdk/sysroots/x86_64-pokysdk-linux")
set(CMAKE_SYSROOT ${POKY_SYSROOT})

# ---------------------------------------------------------------------------
# Compilers
#
# C compiler: standard Poky aarch64 GCC from SDK
# C++ compiler: gcc-reflection cross-compiler targeting aarch64
#
# The reflection branch must be built with aarch64 cross-compilation support.
# Verify with:
#   <gcc-reflection>/bin/g++ --target=aarch64-poky-linux --version
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER
  "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux/aarch64-poky-linux-gcc"
)
set(CMAKE_CXX_COMPILER
  "/aember_ws/aember/yocto/build/aarch64-poky-linux/gcc-reflection/install/bin/aarch64-poky-linux-g++"
)

# Add SDK cross-tools to program path so the reflection g++ finds the correct
# aarch64 assembler and linker instead of the host x86 ones.
# Without this, 'as' resolves to /usr/bin/as (x86) and fails with '-EL' error.
list(APPEND CMAKE_PROGRAM_PATH
  "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux"
)

# ---------------------------------------------------------------------------
# C++ Standard — C++26 with P2996 reflection
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Base flags
#
# Raspberry Pi 4 is Cortex-A72 (ARMv8-A):
#   -march=armv8-a     baseline ARMv8-A instruction set
#   -mcpu=cortex-a72   tune specifically for Cortex-A72
#   -mfpu is not needed on aarch64 — FPU is always present
# ---------------------------------------------------------------------------
set(POKY_FLAGS
  "-march=armv8-a -mcpu=cortex-a72 -fstack-protector-strong"
)
set(REFLECTION_FLAGS "-freflection")

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
set(CMAKE_CXX_FLAGS_INIT        "${POKY_FLAGS} ${REFLECTION_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE     "${RELEASE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RELEASE_LINK_FLAGS}")

# ---------------------------------------------------------------------------
# Post-build strip using aarch64 cross-strip
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  find_program(STRIP_TOOL NAMES aarch64-poky-linux-strip
    HINTS "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux"
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
set(VIRT_ROOT            "/aember_ws/aember/yocto/build/aarch64-poky-linux/virtualization")
set(VIRT_PKG_CONFIG_PATH "${VIRT_ROOT}/pkgconfig")
set(SYSROOT_PKG_CONFIG   "${POKY_SYSROOT}/usr/lib/pkgconfig")

list(APPEND CMAKE_FIND_ROOT_PATH "${VIRT_ROOT}")

set(ENV{PKG_CONFIG_PATH} "${VIRT_PKG_CONFIG_PATH}:${SYSROOT_PKG_CONFIG}:$ENV{PKG_CONFIG_PATH}")

# Prepend aarch64 cross-tools so the reflection g++ finds aarch64-poky-linux-as
# and aarch64-poky-linux-ld rather than the host x86 binaries.
set(ENV{PATH} "${POKY_SDK_ROOT}/usr/bin/aarch64-poky-linux:$ENV{PATH}")

if("${TRIPLET}" MATCHES "poky")
  set(ENV{PKG_CONFIG_SYSROOT_DIR}         "/")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} "1")
  set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS}   "1")
endif()