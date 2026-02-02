# First include vcpkg's toolchain
include("${CMAKE_CURRENT_LIST_DIR}/../../vcpkg/scripts/buildsystems/vcpkg.cmake")

# Use the vcpkg target triplet if available, otherwise default to arm-linux
if(DEFINED VCPKG_TARGET_TRIPLET)
    set(TRIPLET "${VCPKG_TARGET_TRIPLET}")
else()
    set(TRIPLET "arm-linux")
endif()

# Cross-compiler for ARM32
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Specify the cross-compilers
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Extra compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra" CACHE STRING "" FORCE)

# Keep normal subdirectories under the prefix
set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE PATH "Install include dir" FORCE)
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "Install lib dir" FORCE)
set(CMAKE_INSTALL_BINDIR "bin" CACHE PATH "Install bin dir" FORCE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()
