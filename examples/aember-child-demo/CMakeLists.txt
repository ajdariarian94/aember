cmake_minimum_required(VERSION 3.19.5)

# ##############################################################################
#
# Project definition
#
# ##############################################################################
project(
  aember-child-demo
  VERSION ${VERSION}
  HOMEPAGE_URL "https://github.com/ajdariarian94/aember/tree/main/examples/echo"
  DESCRIPTION "echo example"
  LANGUAGES CXX
)

# Dependencies
find_package(
  aember-libs
  REQUIRED
  COMPONENTS utils
             device-health
)

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -no-pie"
)

add_executable(
  ${PROJECT_NAME}
  main.cpp
)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE aember-libs::device-health
          aember-libs::utils::logging
)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>
)

# ##############################################################################
#
# Installation
#
# ##############################################################################

include(GNUInstallDirs)
install(
  TARGETS ${PROJECT_NAME}
  DESTINATION ${CMAKE_INSTALL_PREFIX}/examples
)
