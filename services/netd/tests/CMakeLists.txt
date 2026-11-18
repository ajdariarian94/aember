cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  network-manager-unittest
  src/network-manager.cpp
)

target_link_libraries(
  network-manager-unittest
  PRIVATE aember-netd-core
          GTest::gtest_main
)

add_test(
  NAME network-manager-unittest
  COMMAND network-manager-unittest
)

install(
  TARGETS network-manager-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

add_executable(
  dhcp-client-unittest
  src/dhcp-client.cpp
)

target_link_libraries(
  dhcp-client-unittest
  PRIVATE aember-netd-core
          GTest::gtest_main
)

add_test(
  NAME dhcp-client-unittest
  COMMAND dhcp-client-unittest
)

install(
  TARGETS dhcp-client-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
