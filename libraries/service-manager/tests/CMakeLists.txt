cmake_minimum_required(VERSION 3.16)

find_package(GTest CONFIG REQUIRED)

add_executable(service-manager-unittest src/service-manager.cpp)
add_executable(service-unittest src/service.cpp)

target_include_directories(service-manager-unittest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(service-unittest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(service-manager-unittest
                      PRIVATE aember-libs::service-manager GTest::gtest_main)
target_link_libraries(service-unittest PRIVATE aember-libs::service-manager
                                               GTest::gtest_main)

add_test(NAME service-manager-unittest COMMAND service-manager-unittest)
add_test(NAME service-unittest COMMAND service-unittest)

install(TARGETS service-manager-unittest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests)
install(TARGETS service-unittest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests)
