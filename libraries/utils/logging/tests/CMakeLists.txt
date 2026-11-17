cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  logging-unittest
  src/logging/logging.cpp
)

target_link_libraries(
  logging-unittest
  PRIVATE aember-libs::utils::logging
          GTest::gtest_main
)

add_test(
  NAME logging-unittest
  COMMAND logging-unittest
)

install(
  TARGETS logging-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
