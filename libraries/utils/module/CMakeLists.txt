# -----------------------------------------------------------------------------
# Module subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${MODULE_TARGET}
  INTERFACE
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${MODULE_COMPONENT}
  ALIAS
  ${MODULE_TARGET}
)

target_include_directories(
  ${MODULE_TARGET}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
  ${MODULE_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${MODULE_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
