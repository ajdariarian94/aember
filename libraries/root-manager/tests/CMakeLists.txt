cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  root-manager-unittest
  src/root-manager.cpp
)

target_link_libraries(
  root-manager-unittest
  PRIVATE aember-libs::root-manager
          GTest::gtest_main
)

add_test(
  NAME root-manager-unittest
  COMMAND root-manager-unittest
)

install(
  TARGETS root-manager-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
