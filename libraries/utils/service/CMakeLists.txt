# -----------------------------------------------------------------------------
# Service subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${SERVICE_TARGET}
  STATIC
  src/restart-policy.cpp
  src/service-config.cpp
  src/service-parser.cpp
  src/service-state.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${SERVICE_COMPONENT}
  ALIAS
  ${SERVICE_TARGET}
)

target_include_directories(
  ${SERVICE_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${SERVICE_TARGET}
  PUBLIC aember-libs::utils::config
)

set_target_properties(
  ${SERVICE_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${SERVICE_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  # add_subdirectory(tests)
endif ()
