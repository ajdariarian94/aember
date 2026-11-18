cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  netlink-unittest
  src/netlink.cpp
)

target_link_libraries(
  netlink-unittest
  PRIVATE aember-libs::utils::netlink
          GTest::gtest_main
)

add_test(
  NAME netlink-unittest
  COMMAND netlink-unittest
)

install(
  TARGETS netlink-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
