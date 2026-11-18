# aember-netd: network interfaces (DHCP/static), DNS, connectivity status.

find_package(
  aember-libs
  REQUIRED
  COMPONENTS utils
)

aember_service(
  aember-netd
  MAIN
  src/main.cpp
  SOURCES
  src/dhcp-client.cpp
  src/netd.cpp
  src/network-manager.cpp
  LIBRARIES
  aember-libs::utils::ipc
  aember-libs::utils::logging
  aember-libs::utils::netlink
  nlohmann_json::nlohmann_json
)

if (BUILD_TESTS)
  add_subdirectory(tests)
endif ()
