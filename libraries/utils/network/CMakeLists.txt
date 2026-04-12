# -----------------------------------------------------------------------------
# Network subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${NETWORK_TARGET}
  INTERFACE
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${NETWORK_COMPONENT}
  ALIAS
  ${NETWORK_TARGET}
)

target_include_directories(
  ${NETWORK_TARGET}
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
  ${NETWORK_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${NETWORK_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
