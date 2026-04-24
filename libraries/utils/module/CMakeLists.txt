# -----------------------------------------------------------------------------
# module subcomponent
# -----------------------------------------------------------------------------

add_library(
  ${MODULE_TARGET}
  STATIC
  src/module-parser.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${MODULE_COMPONENT}
  ALIAS
  ${MODULE_TARGET}
)

target_include_directories(
  ${MODULE_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${MODULE_TARGET}
  PUBLIC aember-libs::utils::config
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
