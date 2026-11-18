# -----------------------------------------------------------------------------
# Netlink subcomponent (rtnetlink: links, addresses, routes, bridges, veth)
# -----------------------------------------------------------------------------

add_library(
  ${NETLINK_TARGET}
  STATIC
  src/netlink.cpp
)

add_library(
  ${PROJECT_NAME}::${COMPONENT}::${NETLINK_COMPONENT}
  ALIAS
  ${NETLINK_TARGET}
)

target_include_directories(
  ${NETLINK_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
  ${NETLINK_TARGET}
  PROPERTIES EXPORT_NAME
             ${COMPONENT}::${NETLINK_COMPONENT}
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif ()
