# -----------------------------------------------------------------------------
# IPC subcomponent (daemon <-> PID 1 channels)
# -----------------------------------------------------------------------------

add_library(
  ${IPC_TARGET}
  STATIC
  src/channel.cpp
  src/hub.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${IPC_COMPONENT}
  ALIAS
  ${IPC_TARGET}
)

target_include_directories(
  ${IPC_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${IPC_TARGET}
  PUBLIC aember-libs::utils::logging
         nlohmann_json::nlohmann_json
)

set_target_properties(
  ${IPC_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${IPC_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif ()
