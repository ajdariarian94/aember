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

# The daemon end to end: RunNetd() in a forked child with a channel on fd 3
add_executable(
  netd-daemon-unittest
  src/netd.cpp
)

target_link_libraries(
  netd-daemon-unittest
  PRIVATE aember-netd-core
          GTest::gtest_main
)

add_test(
  NAME netd-daemon-unittest
  COMMAND netd-daemon-unittest
)

install(
  TARGETS netd-daemon-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
