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

# Set language standards
set(CMAKE_C_STANDARD
    11
)
set(CMAKE_CXX_STANDARD
    17
)
set(CMAKE_EXPORT_COMPILE_COMMANDS
    ON
)

# Examples
add_subdirectory(echo-aember)
