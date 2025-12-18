cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

add_executable(logging-unittest src/logging/logging.cpp)
add_executable(signal-unittest src/signal/signal.cpp)

target_include_directories(logging-unittest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(signal-unittest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(logging-unittest PRIVATE aember-libs::utils::logging
                                               GTest::gtest_main)
target_link_libraries(signal-unittest PRIVATE aember-libs::utils::signal
                                              GTest::gtest_main)

add_test(NAME logging-unittest COMMAND logging-unittest)
add_test(NAME signal-unittest COMMAND signal-unittest)

install(TARGETS logging-unittest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests)
install(TARGETS signal-unittest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests)
