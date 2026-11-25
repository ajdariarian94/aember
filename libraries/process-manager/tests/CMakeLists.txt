cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  process-manager-unittest
  src/process-manager.cpp
  src/restart-properties.cpp
)

target_include_directories(
  process-manager-unittest
  PRIVATE ${AEMBER_TEST_SUPPORT_DIR}
)

target_link_libraries(
  process-manager-unittest
  PRIVATE aember-libs::process-manager
          aember-libs::child-supervisor
          GTest::gtest_main
)

add_test(
  NAME process-manager-unittest
  COMMAND process-manager-unittest
)

install(
  TARGETS process-manager-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
