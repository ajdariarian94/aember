cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  ipc-unittest
  src/ipc.cpp
  src/ipc-properties.cpp
)

target_include_directories(
  ipc-unittest
  PRIVATE ${AEMBER_TEST_SUPPORT_DIR}
)

target_link_libraries(
  ipc-unittest
  PRIVATE aember-libs::utils::ipc
          GTest::gtest_main
)

add_test(
  NAME ipc-unittest
  COMMAND ipc-unittest
)

install(
  TARGETS ipc-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
