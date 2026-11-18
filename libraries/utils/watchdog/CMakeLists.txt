# -----------------------------------------------------------------------------
# Watchdog subcomponent (/dev/watchdog: conditional feeding, magic close)
# -----------------------------------------------------------------------------

add_library(
  ${WATCHDOG_TARGET}
  STATIC
  src/watchdog.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${WATCHDOG_COMPONENT}
  ALIAS
  ${WATCHDOG_TARGET}
)

target_include_directories(
  ${WATCHDOG_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${WATCHDOG_TARGET}
  PUBLIC aember-libs::utils::logging
)

set_target_properties(
  ${WATCHDOG_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${WATCHDOG_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif ()
