set(COMPONENT "child-supervisor")

set(${COMPONENT}_${PROJECT_NAME}_VERSION ${VERSION})

# ##############################################################################
#
# Library
#
# ##############################################################################

add_library(${COMPONENT} STATIC src/child-supervisor.cpp)

add_library(${PROJECT_NAME}::${COMPONENT} ALIAS ${COMPONENT})

target_include_directories(
  ${COMPONENT} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(${COMPONENT} PUBLIC aember-libs::utils::logging)

set_target_properties(
  ${COMPONENT} PROPERTIES VERSION ${${COMPONENT}_${PROJECT_NAME}_VERSION}
                          OUTPUT_NAME ${COMPONENT}_${PROJECT_NAME})

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
  VERSION ${${COMPONENT}_${PROJECT_NAME}_VERSION}
  COMPATIBILITY AnyNewerVersion)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${COMPONENT})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS ${COMPONENT} EXPORT ${COMPONENT})

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT})

install(
  EXPORT ${COMPONENT}
  FILE ${COMPONENT}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT})

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if(BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif()
