# -----------------------------------------------------------------------------
# Root subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${ROOT_TARGET}
  STATIC
  src/root-config.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${ROOT_COMPONENT}
  ALIAS
  ${ROOT_TARGET}
)

target_include_directories(
  ${ROOT_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
  ${ROOT_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${ROOT_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
