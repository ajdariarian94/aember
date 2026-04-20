cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  signal-unittest
  src/signal/signal.cpp
)

target_include_directories(
  signal-unittest
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  signal-unittest
  PRIVATE aember-libs::utils::signal
          GTest::gtest_main
)

add_test(
  NAME signal-unittest
  COMMAND signal-unittest
)

install(
  TARGETS signal-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
