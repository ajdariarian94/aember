cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  container-manager-unittest
  src/container-manager.cpp
)

target_link_libraries(
  container-manager-unittest
  PRIVATE aember-libs::container-manager
          aember-libs::mount-manager
          GTest::gtest_main
)

add_test(
  NAME container-manager-unittest
  COMMAND container-manager-unittest
)

install(
  TARGETS container-manager-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
