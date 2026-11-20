cmake_minimum_required(VERSION 3.19.5)

# ##############################################################################
#
# Project definition
#
# ##############################################################################
project(
  aember-executables
  VERSION ${VERSION}
  DESCRIPTION "aember-executables project"
  LANGUAGES CXX
)

# Warnings (as errors) for aember's own code
include(${CMAKE_CURRENT_SOURCE_DIR}/../tools/cmake/AemberWarnings.cmake)

# Executables
add_subdirectory(aember)
