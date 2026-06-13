cmake_minimum_required(VERSION 3.16)

set(COMPONENT
    "utils"
)
set(${COMPONENT}_${PROJECT_NAME}_VERSION
    ${VERSION}
)

# -----------------------------------------------------------------------------
# Subcomponents
# -----------------------------------------------------------------------------
set(SUBCOMPONENTS
    config
    container
    logging
    module
    mount
    network
    process
    root
    # service
    shell
    signal
)

# Set up variables that subcomponents expect (preserving original naming)
foreach (
  subcomp
  IN
  LISTS SUBCOMPONENTS
)
  string(
    TOUPPER ${subcomp}
            SUBCOMP_UPPER
  )
  set(${SUBCOMP_UPPER}_COMPONENT
      ${subcomp}
  )
  set(${SUBCOMP_UPPER}_TARGET
      "${COMPONENT}_${subcomp}"
  )
endforeach ()

# Now add subdirectories with all variables properly set
foreach (
  subcomp
  IN
  LISTS SUBCOMPONENTS
)
  add_subdirectory(${subcomp})
  string(
    TOUPPER ${subcomp}
            SUBCOMP_UPPER
  )
  list(
    APPEND
    SUBCOMPONENT_TARGETS
    ${${SUBCOMP_UPPER}_TARGET}
  )
endforeach ()

# -----------------------------------------------------------------------------
# Umbrella target
# -----------------------------------------------------------------------------
add_library(
  ${COMPONENT}
  INTERFACE
)
add_library(
  ${PROJECT_NAME}::${COMPONENT}
  ALIAS
  ${COMPONENT}
)

target_link_libraries(
  ${COMPONENT}
  INTERFACE ${LOGGING_TARGET}
)
set_target_properties(
  ${COMPONENT}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}
)

# -----------------------------------------------------------------------------
# Installation
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Package config
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
  VERSION ${${COMPONENT}_${PROJECT_NAME}_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT}
)

# Install headers
foreach (
  subcomp
  IN
  LISTS SUBCOMPONENTS
)
  install(
    DIRECTORY ${subcomp}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
endforeach ()

# Install targets
install(
  TARGETS ${COMPONENT}
          ${SUBCOMPONENT_TARGETS}
  EXPORT ${COMPONENT}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Export and config files
install(
  EXPORT ${COMPONENT}
  FILE ${COMPONENT}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT}
)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT}
)
