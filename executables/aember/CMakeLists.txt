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
    "https://github.com/ajdariarian94/aember/tree/main/executables/aember"
  DESCRIPTION "aember executable"
  LANGUAGES CXX)

# Dependencies
find_package(aember-libs REQUIRED COMPONENTS device-health)

find_package(spdlog REQUIRED)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE aember-libs::device-health)

target_include_directories(
  ${PROJECT_NAME} PRIVATE $<BUILD_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>)

# ##############################################################################
#
# Installation
#
# ##############################################################################

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
