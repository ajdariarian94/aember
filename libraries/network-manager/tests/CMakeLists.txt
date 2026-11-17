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
  PRIVATE aember-libs::network-manager
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
  dhcp-netlink-unittest
  src/dhcp-netlink.cpp
)

target_link_libraries(
  dhcp-netlink-unittest
  PRIVATE aember-libs::network-manager
          GTest::gtest_main
)

add_test(
  NAME dhcp-netlink-unittest
  COMMAND dhcp-netlink-unittest
)

install(
  TARGETS dhcp-netlink-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
