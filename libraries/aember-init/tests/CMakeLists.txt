cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  pid1-unittest
  src/pid1.cpp
)

target_link_libraries(
  pid1-unittest
  PRIVATE aember-libs::aember-init
          GTest::gtest_main
)

add_test(
  NAME pid1-unittest
  COMMAND pid1-unittest
)

install(
  TARGETS pid1-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
