cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  config-manager-unittest
  src/config-manager.cpp
)

target_link_libraries(
  config-manager-unittest
  PRIVATE aember-libs::utils::config
          GTest::gtest_main
)

add_test(
  NAME config-manager-unittest
  COMMAND config-manager-unittest
)

install(
  TARGETS config-manager-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
