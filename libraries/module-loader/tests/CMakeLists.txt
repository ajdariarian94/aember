cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  module-loader-unittest
  src/module-loader.cpp
)

target_link_libraries(
  module-loader-unittest
  PRIVATE aember-libs::module-loader
          GTest::gtest_main
)

add_test(
  NAME module-loader-unittest
  COMMAND module-loader-unittest
)

install(
  TARGETS module-loader-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
