cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  process-config-parser-unittest
  src/process-config-parser.cpp
)

target_link_libraries(
  process-config-parser-unittest
  PRIVATE aember-libs::utils::process
          GTest::gtest_main
)

add_test(
  NAME process-config-parser-unittest
  COMMAND process-config-parser-unittest
)

install(
  TARGETS process-config-parser-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
