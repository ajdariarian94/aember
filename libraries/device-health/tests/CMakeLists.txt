cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  heartbeat-unittest
  src/heartbeat.cpp
)

target_include_directories(
  heartbeat-unittest
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  heartbeat-unittest
  PRIVATE aember-libs::device-health
          GTest::gtest_main
)

add_test(
  NAME heartbeat-unittest
  COMMAND heartbeat-unittest
)

install(
  TARGETS heartbeat-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

add_executable(
  aember-monitor-unittest
  src/aember-monitor.cpp
)

target_link_libraries(
  aember-monitor-unittest
  PRIVATE aember-libs::device-health
          GTest::gtest_main
)

add_test(
  NAME aember-monitor-unittest
  COMMAND aember-monitor-unittest
)

install(
  TARGETS aember-monitor-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
