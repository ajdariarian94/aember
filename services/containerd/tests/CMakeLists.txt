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
  PRIVATE aember-containerd-core
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

add_executable(
  container-parser-unittest
  src/container-parser.cpp
  src/container-parser-properties.cpp
)

target_include_directories(
  container-parser-unittest
  PRIVATE ${AEMBER_TEST_SUPPORT_DIR}
)

target_link_libraries(
  container-parser-unittest
  PRIVATE aember-containerd-core
          GTest::gtest_main
)

add_test(
  NAME container-parser-unittest
  COMMAND container-parser-unittest
)

install(
  TARGETS container-parser-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

add_executable(
  init-wrapper-unittest
  src/init-wrapper.cpp
)

target_link_libraries(
  init-wrapper-unittest
  PRIVATE aember-containerd-core
          GTest::gtest_main
)

add_test(
  NAME init-wrapper-unittest
  COMMAND init-wrapper-unittest
)

install(
  TARGETS init-wrapper-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

# The daemon end to end: RunContainerd() in a forked child with a channel on fd
# 3
add_executable(
  containerd-daemon-unittest
  src/containerd.cpp
)

target_link_libraries(
  containerd-daemon-unittest
  PRIVATE aember-containerd-core
          GTest::gtest_main
)

add_test(
  NAME containerd-daemon-unittest
  COMMAND containerd-daemon-unittest
)

install(
  TARGETS containerd-daemon-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

# ContainerManager's LXC lifecycle against a fake liblxc (no root, no mounts)
add_executable(
  container-lifecycle-unittest
  src/container-lifecycle.cpp
)

target_include_directories(
  container-lifecycle-unittest
  PRIVATE ${LXC_INCLUDE_DIR}
)

target_link_libraries(
  container-lifecycle-unittest
  PRIVATE aember-containerd-core
          GTest::gtest_main
)

add_test(
  NAME container-lifecycle-unittest
  COMMAND container-lifecycle-unittest
)

install(
  TARGETS container-lifecycle-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

add_executable(
  control-unittest
  src/control.cpp
)

target_link_libraries(
  control-unittest
  PRIVATE aember-containerd-core
          GTest::gtest_main
)

add_test(
  NAME control-unittest
  COMMAND control-unittest
)

install(
  TARGETS control-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
