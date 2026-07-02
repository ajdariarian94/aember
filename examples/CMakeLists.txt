cmake_minimum_required(VERSION 3.19.5)

# ##############################################################################
#
# Project definition
#
# ##############################################################################
project(
  aember-examples
  VERSION ${VERSION}
  DESCRIPTION "aember-examples project"
  LANGUAGES CXX
)

# Examples
add_subdirectory(aember-child-demo)
