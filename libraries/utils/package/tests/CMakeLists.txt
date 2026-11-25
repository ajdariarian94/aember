cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  package-unittest
  src/package.cpp
  src/install.cpp
)

target_include_directories(
  package-unittest
  PRIVATE ${AEMBER_TEST_SUPPORT_DIR}
)

target_compile_definitions(
  package-unittest
  PRIVATE AEMBER_PACKAGE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(
  package-unittest
  PRIVATE aember-libs::utils::package
          GTest::gtest_main
)

add_test(
  NAME package-unittest
  COMMAND package-unittest
)

install(
  TARGETS package-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
