# -----------------------------------------------------------------------------
# Process subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${PROCESS_TARGET}
  STATIC
  src/process-config-parser.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${PROCESS_COMPONENT}
  ALIAS
  ${PROCESS_TARGET}
)

target_include_directories(
  ${PROCESS_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${PROCESS_TARGET}
  PUBLIC aember-libs::utils::config
)

set_target_properties(
  ${PROCESS_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${PROCESS_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
