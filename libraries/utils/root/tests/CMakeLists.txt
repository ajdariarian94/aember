cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  root-config-unittest
  src/root-config.cpp
)

target_link_libraries(
  root-config-unittest
  PRIVATE aember-libs::utils::root
          GTest::gtest_main
)

add_test(
  NAME root-config-unittest
  COMMAND root-config-unittest
)

install(
  TARGETS root-config-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
