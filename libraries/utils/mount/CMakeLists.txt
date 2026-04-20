# -----------------------------------------------------------------------------
# Mount subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${MOUNT_TARGET}
  INTERFACE
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${MOUNT_COMPONENT}
  ALIAS
  ${MOUNT_TARGET}
)

target_include_directories(
  ${MOUNT_TARGET}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
  ${MOUNT_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${MOUNT_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
