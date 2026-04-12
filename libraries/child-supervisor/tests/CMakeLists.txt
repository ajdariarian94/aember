cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  child-supervisor-unittest
  src/child-supervisor.cpp
)

target_include_directories(
  child-supervisor-unittest
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  child-supervisor-unittest
  PRIVATE aember-libs::child-supervisor
          GTest::gtest_main
)

add_test(
  NAME child-supervisor-unittest
  COMMAND child-supervisor-unittest
)

install(
  TARGETS child-supervisor-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
