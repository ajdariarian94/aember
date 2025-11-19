cmake_minimum_required(VERSION 3.16)

# Component name
set(COMPONENT "utils")

# Version (assumes VERSION and PROJECT_NAME provided by parent)
set(${COMPONENT}_${PROJECT_NAME}_VERSION ${VERSION})

# -----------------------------------------------------------------------------
# Logging subcomponent
# -----------------------------------------------------------------------------

set(LOGGING_COMPONENT "logging")
set(LOGGING_TARGET "${COMPONENT}_${LOGGING_COMPONENT}") # utils_logging

add_library(${LOGGING_TARGET} STATIC src/logging/logging.cpp)

add_library(${PROJECT_NAME}::${COMPONENT}::${LOGGING_COMPONENT} ALIAS
            ${LOGGING_TARGET})

target_include_directories(
  ${LOGGING_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(${LOGGING_TARGET} PUBLIC spdlog::spdlog)

set_target_properties(${LOGGING_TARGET}
                      PROPERTIES EXPORT_NAME ${COMPONENT}::${LOGGING_COMPONENT})

# -----------------------------------------------------------------------------
# Umbrella component target (empty)
# -----------------------------------------------------------------------------

add_library(${COMPONENT} INTERFACE)
add_library(${PROJECT_NAME}::${COMPONENT} ALIAS ${COMPONENT})

# Umbrella depends on all subcomponents
target_link_libraries(${COMPONENT} INTERFACE ${LOGGING_TARGET})

set_target_properties(${COMPONENT} PROPERTIES EXPORT_NAME ${COMPONENT})

# -----------------------------------------------------------------------------
# Installation
# -----------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Config + Version files
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
  VERSION ${${COMPONENT}_${PROJECT_NAME}_VERSION}
  COMPATIBILITY AnyNewerVersion)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT}
)

# Install headers
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install targets
install(
  TARGETS ${COMPONENT} ${LOGGING_TARGET}
  EXPORT ${COMPONENT}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Export target set
install(
  EXPORT ${COMPONENT}
  FILE ${COMPONENT}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT})

# Install config & version files
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}Config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT}ConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${COMPONENT})
