cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  watchdog-unittest
  src/watchdog.cpp
)

target_link_libraries(
  watchdog-unittest
  PRIVATE aember-libs::utils::watchdog
          GTest::gtest_main
)

add_test(
  NAME watchdog-unittest
  COMMAND watchdog-unittest
)

install(
  TARGETS watchdog-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
