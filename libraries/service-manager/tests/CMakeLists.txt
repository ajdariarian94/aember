cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  service-manager-unittest
  src/service-manager.cpp
)

target_link_libraries(
  service-manager-unittest
  PRIVATE aember-libs::service-manager
          aember-libs::process-manager
          aember-libs::container-manager
          aember-libs::child-supervisor
          aember-libs::mount-manager
          GTest::gtest_main
)

add_test(
  NAME service-manager-unittest
  COMMAND service-manager-unittest
)

install(
  TARGETS service-manager-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
