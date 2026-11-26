# -----------------------------------------------------------------------------
# Package subcomponent: the .aember format (verify, unpack, install)
# -----------------------------------------------------------------------------

find_package(
  LibLZMA
  REQUIRED
)

# Monocypher (third_party/monocypher, BSD-2-Clause): SHA-512 and Ed25519. Built
# as C++ inside this library, without aember's warnings (not our code).
set(MONOCYPHER_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/monocypher
)
set(MONOCYPHER_SOURCES
    ${MONOCYPHER_DIR}/monocypher.c
    ${MONOCYPHER_DIR}/monocypher-ed25519.c
)
set_source_files_properties(
  ${MONOCYPHER_SOURCES}
  PROPERTIES LANGUAGE
             CXX
             COMPILE_OPTIONS
             "-w"
)

add_library(
  ${PACKAGE_TARGET}
  STATIC
  src/install.cpp
  src/package.cpp
  src/tar.cpp
  src/writer.cpp
  ${MONOCYPHER_SOURCES}
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${PACKAGE_COMPONENT}
  ALIAS
  ${PACKAGE_TARGET}
)

target_include_directories(
  ${PACKAGE_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${MONOCYPHER_DIR}
)

target_link_libraries(
  ${PACKAGE_TARGET}
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE LibLZMA::LibLZMA
)

set_target_properties(
  ${PACKAGE_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${PACKAGE_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif ()
