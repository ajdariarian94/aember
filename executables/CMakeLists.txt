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

# Executables
add_subdirectory(aember)
