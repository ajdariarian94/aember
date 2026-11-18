cmake_minimum_required(VERSION 3.19.5)

# ##############################################################################
#
# Project definition
#
# Services are separate programs PID 1 starts from services.json. Each directory
# here with a CMakeLists.txt is one service; it builds against the installed
# aember-libs (find_package) and installs its executable into
# ${CMAKE_INSTALL_PREFIX}/services, from where `aember app pack` copies the ones
# a board's services.json lists. See services/README.md.
#
# ##############################################################################
project(
  aember-services
  VERSION ${VERSION}
  DESCRIPTION "aember services (programs started by PID 1)"
  LANGUAGES CXX
)

find_package(
  nlohmann_json
  REQUIRED
)
find_package(
  spdlog
  REQUIRED
)

include(GNUInstallDirs)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AemberService.cmake)

if (BUILD_TESTS)
  include(CTest)
  enable_testing()
endif ()

# Every subdirectory with a CMakeLists.txt is a service.
file(
  GLOB
  children
  LIST_DIRECTORIES true
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/*
)
list(
  SORT
  children
)
foreach (
  child
  IN
  LISTS children
)
  if (EXISTS
      ${CMAKE_CURRENT_SOURCE_DIR}/${child}/CMakeLists.txt
  )
    message(STATUS "aember service: ${child}")
    add_subdirectory(${child})
  endif ()
endforeach ()
