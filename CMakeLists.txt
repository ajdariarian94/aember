cmake_minimum_required(VERSION 3.19.5)

# ##############################################################################
#
# Project definition
#
# ##############################################################################
project(
  aember
  VERSION 0.5.0
  HOMEPAGE_URL "https://github.com/ajdariarian94/aember.git"
  DESCRIPTION "aember superbuild project"
  LANGUAGES C CXX)

# ##############################################################################
#
# External project definition
#
# ##############################################################################
include(ExternalProject)

# It is necessary to pass vcpkg variables to subprojects. The following
# variables are important: VCPKG_HOST_TRIPLET VCPKG_TARGET_TRIPLET
# VCPKG_INSTALLED_DIR VCPKG_MANIFEST_INSTALL

ExternalProject_Add(
  aember-libs
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libraries
  BUILD_ALWAYS TRUE
  CMAKE_ARGS -DVCPKG_HOST_TRIPLET=${VCPKG_HOST_TRIPLET}
             -DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}
             -DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}
             -DVCPKG_MANIFEST_INSTALL=${VCPKG_MANIFEST_INSTALL}
             -DCMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
             -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
             -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}
             -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DBUILD_TESTS=${BUILD_TESTS}
             -DVERSION=${PROJECT_VERSION})

ExternalProject_Add(
  executables
  DEPENDS aember-libs
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/executables
  BUILD_ALWAYS TRUE
  CMAKE_ARGS -DVCPKG_HOST_TRIPLET=${VCPKG_HOST_TRIPLET}
             -DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}
             -DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}
             -DVCPKG_MANIFEST_INSTALL=${VCPKG_MANIFEST_INSTALL}
             -DCMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
             -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
             -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}
             -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DVERSION=${PROJECT_VERSION})

ExternalProject_Add(
  examples
  DEPENDS aember-libs
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/examples
  BUILD_ALWAYS TRUE
  CMAKE_ARGS -DVCPKG_HOST_TRIPLET=${VCPKG_HOST_TRIPLET}
             -DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}
             -DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}
             -DVCPKG_MANIFEST_INSTALL=${VCPKG_MANIFEST_INSTALL}
             -DCMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
             -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
             -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}
             -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DVERSION=${PROJECT_VERSION})
