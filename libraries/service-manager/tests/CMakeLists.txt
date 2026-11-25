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
          aember-libs::child-supervisor
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

# Property tests on random dependency graphs (aember-test/dependency-graph.h)
add_executable(
  dependency-resolver-unittest
  src/dependency-resolver.cpp
)

target_include_directories(
  dependency-resolver-unittest
  PRIVATE ${AEMBER_TEST_SUPPORT_DIR}
)

target_link_libraries(
  dependency-resolver-unittest
  PRIVATE aember-libs::service-manager
          GTest::gtest_main
)

add_test(
  NAME dependency-resolver-unittest
  COMMAND dependency-resolver-unittest
)

install(
  TARGETS dependency-resolver-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
