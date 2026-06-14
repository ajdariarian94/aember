# -----------------------------------------------------------------------------
# Shell subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${SHELL_TARGET}
  STATIC
  src/debug-shell.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${SHELL_COMPONENT}
  ALIAS
  ${SHELL_TARGET}
)

target_include_directories(
  ${SHELL_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${SHELL_TARGET}
  PUBLIC aember-libs::utils::logging
)

set_target_properties(
  ${SHELL_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${SHELL_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
