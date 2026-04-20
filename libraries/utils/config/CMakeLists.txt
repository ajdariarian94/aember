# -----------------------------------------------------------------------------
# Config subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${CONFIG_TARGET}
  STATIC
  src/config-manager.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${CONFIG_COMPONENT}
  ALIAS
  ${CONFIG_TARGET}
)

target_include_directories(
  ${CONFIG_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${CONFIG_TARGET}
  PUBLIC aember-libs::utils::logging
         aember-libs::utils::service
         nlohmann_json::nlohmann_json
)

set_target_properties(
  ${CONFIG_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${CONFIG_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif ()
