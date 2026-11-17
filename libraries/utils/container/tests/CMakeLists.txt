cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  container-parser-unittest
  src/container-parser.cpp
)

target_link_libraries(
  container-parser-unittest
  PRIVATE aember-libs::utils::container
          GTest::gtest_main
)

add_test(
  NAME container-parser-unittest
  COMMAND container-parser-unittest
)

install(
  TARGETS container-parser-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
