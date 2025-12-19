cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

add_executable(mount-manager-unittest src/mount-manager.cpp)

target_include_directories(mount-manager-unittest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(mount-manager-unittest PRIVATE aember-libs::mount-manager
                                                     GTest::gtest_main)

add_test(NAME mount-manager-unittest COMMAND mount-manager-unittest)

install(TARGETS mount-manager-unittest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests)
