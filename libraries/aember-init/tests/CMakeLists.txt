cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  pid1-unittest
  src/pid1.cpp
)

target_link_libraries(
  pid1-unittest
  PRIVATE aember-libs::aember-init
          GTest::gtest_main
)

add_test(
  NAME pid1-unittest
  COMMAND pid1-unittest
)

install(
  TARGETS pid1-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)

# Integration: AemberInit::StartRoot() with real services in a forked child
find_package(
  nlohmann_json
  CONFIG
  REQUIRED
)

add_executable(
  aember-init-unittest
  src/aember-init.cpp
)

target_link_libraries(
  aember-init-unittest
  PRIVATE aember-libs::aember-init
          nlohmann_json::nlohmann_json
          GTest::gtest_main
)

add_test(
  NAME aember-init-unittest
  COMMAND aember-init-unittest
)

install(
  TARGETS aember-init-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
