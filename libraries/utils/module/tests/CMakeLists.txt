cmake_minimum_required(VERSION 3.16)

find_package(
  GTest
  CONFIG
  REQUIRED
)

add_executable(
  module-parser-unittest
  src/module-parser.cpp
  src/module-properties.cpp
)

target_include_directories(
  module-parser-unittest
  PRIVATE ${AEMBER_TEST_SUPPORT_DIR}
)

target_link_libraries(
  module-parser-unittest
  PRIVATE aember-libs::utils::module
          GTest::gtest_main
)

add_test(
  NAME module-parser-unittest
  COMMAND module-parser-unittest
)

install(
  TARGETS module-parser-unittest
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
)
