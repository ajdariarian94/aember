cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  debug-shell-unittest
  src/debug-shell.cpp
)

target_link_libraries(
  debug-shell-unittest
  PRIVATE aember-libs::utils::shell
          GTest::gtest_main
)

add_test(
  NAME debug-shell-unittest
  COMMAND debug-shell-unittest
)

install(
  TARGETS debug-shell-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
