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
  LANGUAGES CXX
)

# Dependencies
find_package(
  CLI11
  CONFIG
  REQUIRED
)
find_package(
  aember-libs
  REQUIRED
  COMPONENTS aember-init
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
  PRIVATE aember-libs::aember-init
          CLI11::CLI11
)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>
)

target_compile_features(
  ${PROJECT_NAME}
  PRIVATE cxx_std_26
)

target_link_options(
  ${PROJECT_NAME}
  PRIVATE
  -static-libstdc++
  -static-libgcc
)

# ##############################################################################
#
# Installation
#
# ##############################################################################

include(GNUInstallDirs)
install(
  TARGETS ${PROJECT_NAME}
  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)
