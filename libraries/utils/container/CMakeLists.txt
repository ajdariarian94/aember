# -----------------------------------------------------------------------------
# Container subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${CONTAINER_TARGET}
  STATIC
  src/container-state.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${CONTAINER_COMPONENT}
  ALIAS
  ${CONTAINER_TARGET}
)

target_include_directories(
  ${CONTAINER_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
  ${CONTAINER_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${CONTAINER_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
