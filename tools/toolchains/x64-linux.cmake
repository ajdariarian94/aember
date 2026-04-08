# =====================================
# vcpkg toolchain (must be first)
# =====================================
include("${CMAKE_CURRENT_LIST_DIR}/../../vcpkg/scripts/buildsystems/vcpkg.cmake")

# =====================================
# Target triplet
# =====================================
if(DEFINED VCPKG_TARGET_TRIPLET)
    set(TRIPLET "${VCPKG_TARGET_TRIPLET}")
else()
    set(TRIPLET "x64-linux")
endif()

# =====================================
# pkg-config search paths
# =====================================
set(ENV{PKG_CONFIG_PATH}
    "${CMAKE_CURRENT_LIST_DIR}/../../build/${TRIPLET}/virtualization/pkgconfig:$ENV{PKG_CONFIG_PATH}"
)

# =====================================
# Compilers
# =====================================
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

# =====================================
# Common compiler flags (all configs)
# =====================================
set(COMMON_CXX_FLAGS
    "-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion"
)

# =====================================
# Debug configuration (REAL debugging)
# =====================================
set(CMAKE_CXX_FLAGS_DEBUG
    "${COMMON_CXX_FLAGS} \
     -Og -g3 \
     -fno-omit-frame-pointer \
     -fstack-protector-strong \
     -D_GLIBCXX_ASSERTIONS \
     -fsanitize=address,undefined"
    CACHE STRING "" FORCE
)

set(CMAKE_C_FLAGS_DEBUG
    "-Og -g3 \
     -fno-omit-frame-pointer \
     -fstack-protector-strong \
     -fsanitize=address,undefined"
    CACHE STRING "" FORCE
)

# =====================================
# Release configuration (MAX PERFORMANCE)
# =====================================
set(CMAKE_CXX_FLAGS_RELEASE
    "${COMMON_CXX_FLAGS} \
     -O3 \
     -march=native \
     -mtune=native \
     -flto \
     -ffast-math \
     -funroll-loops \
     -fomit-frame-pointer \
     -DNDEBUG"
    CACHE STRING "" FORCE
)

set(CMAKE_C_FLAGS_RELEASE
    "-O3 \
     -march=native \
     -mtune=native \
     -flto \
     -ffast-math \
     -funroll-loops \
     -fomit-frame-pointer \
     -DNDEBUG"
    CACHE STRING "" FORCE
)

# =====================================
# Linker flags (LTO)
# =====================================
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-flto" CACHE STRING "" FORCE)

# =====================================
# Install directories
# =====================================
set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE PATH "Install include dir" FORCE)
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "Install lib dir" FORCE)
set(CMAKE_INSTALL_BINDIR "bin" CACHE PATH "Install bin dir" FORCE)

# =====================================
# spdlog verbosity
# =====================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()
