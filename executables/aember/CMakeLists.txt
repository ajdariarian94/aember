cmake_minimum_required(VERSION 3.19.5)

# ##############################################################################
#
# Project definition
#
# ##############################################################################
project(
  aember
  VERSION ${VERSION}
  HOMEPAGE_URL
    "https://github.com/cybee-ai/cybee/tree/main/executables/cybee-xdr"
  DESCRIPTION "aember executable"
  LANGUAGES CXX)

# Dependencies
find_package(aember-libs REQUIRED COMPONENTS device_health)

find_package(spdlog REQUIRED)

# Force static linking of stdlib (if available)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)

# Add static flags explicitly
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -no-pie")

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE aember-libs::device_health)

target_include_directories(
  ${PROJECT_NAME} PRIVATE $<BUILD_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>)

# ##############################################################################
#
# Installation
#
# ##############################################################################

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
