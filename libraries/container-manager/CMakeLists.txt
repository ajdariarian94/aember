set(COMPONENT
    "container-manager"
)

set(${COMPONENT}_${PROJECT_NAME}_VERSION
    ${VERSION}
)

if (NOT
    DEFINED
    ARCHITECTURE
)
  message(
    FATAL_ERROR
      "ARCHITECTURE must be defined (e.g. -DARCHITECTURE=x64-poky-linux)"
  )
endif ()

find_package(
  PkgConfig
  REQUIRED
)

set(LXC_INCLUDE_DIR
    "/aember_ws/aember/yocto/build/${ARCHITECTURE}/virtualization/include"
)
set(LXC_LIBRARY
    "/aember_ws/aember/yocto/build/${ARCHITECTURE}/virtualization/libs/liblxc.so"
)

if (NOT
    EXISTS
    ${LXC_INCLUDE_DIR}/lxc/lxccontainer.h
)
  message(FATAL_ERROR "lxccontainer.h not found at ${LXC_INCLUDE_DIR}")
endif ()

if (NOT
    EXISTS
    ${LXC_LIBRARY}
)
  message(FATAL_ERROR "liblxc.so not found at ${LXC_LIBRARY}")
endif ()

add_library(
  LXC::LXC
  SHARED
  IMPORTED
)
set_target_properties(
  LXC::LXC
  PROPERTIES IMPORTED_LOCATION
             "${LXC_LIBRARY}"
             INTERFACE_INCLUDE_DIRECTORIES
             "${LXC_INCLUDE_DIR}"
)

# ##############################################################################
#
# Library
#
# ##############################################################################

add_library(
  ${COMPONENT}
  STATIC
  src/container-manager.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}
  ALIAS
  ${COMPONENT}
)

target_include_directories(
  ${COMPONENT}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  ${COMPONENT}
  PUBLIC aember-libs::mount-manager
         aember-libs::utils::container
         aember-libs::utils::logging
         LXC::LXC
)

set_target_properties(
  ${COMPONENT}
  PROPERTIES VERSION
             ${${COMPONENT}_${PROJECT_NAME}_VERSION}
             OUTPUT_NAME
             ${COMPONENT}_${PROJECT_NAME}
)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
  VERSION ${${COMPONENT}_${PROJECT_NAME}_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${COMPONENT}
)

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  TARGETS ${COMPONENT}
  EXPORT ${COMPONENT}
)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT}
)

install(
  EXPORT ${COMPONENT}
  FILE ${COMPONENT}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

# if(BUILD_TESTS) include(CTest) enable_testing()

# add_subdirectory(tests) endif()
