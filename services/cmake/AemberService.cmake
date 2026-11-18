# aember_service(<name>
#                MAIN <main.cpp>
#                [SOURCES <src>...]       # everything but main: <name>-core
#                [LIBRARIES <target>...]  # e.g. aember-libs::utils::ipc
#                [PRIVATE_INCLUDES <dir>...])
#
# Builds the service executable <name> (installed to
# ${CMAKE_INSTALL_PREFIX}/services/<name>, stripped in Release). With
# SOURCES, the code is also a static library <name>-core (include/ is its
# public include dir) that the service's tests link against.
function (aember_service name)
  cmake_parse_arguments(
    ARG
    ""
    "MAIN"
    "SOURCES;LIBRARIES;PRIVATE_INCLUDES"
    ${ARGN}
  )
  if (NOT ARG_MAIN)
    message(FATAL_ERROR "aember_service(${name}): MAIN is required")
  endif ()

  add_executable(
    ${name}
    ${ARG_MAIN}
  )

  if (ARG_SOURCES)
    add_library(
      ${name}-core
      STATIC
      ${ARG_SOURCES}
    )
    target_include_directories(
      ${name}-core
      PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE ${ARG_PRIVATE_INCLUDES}
    )
    target_link_libraries(
      ${name}-core
      PUBLIC ${ARG_LIBRARIES}
    )
    target_link_libraries(
      ${name}
      PRIVATE ${name}-core
    )
  else ()
    target_include_directories(
      ${name}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
              ${ARG_PRIVATE_INCLUDES}
    )
    target_link_libraries(
      ${name}
      PRIVATE ${ARG_LIBRARIES}
    )
  endif ()

  # Same link mode as the aember binary.
  target_link_options(
    ${name}
    PRIVATE -no-pie
  )

  if (CMAKE_BUILD_TYPE
      STREQUAL
      "Release"
      AND DEFINED
          AEMBER_STRIP_COMMAND
  )
    add_custom_command(
      TARGET ${name}
      POST_BUILD
      COMMAND ${AEMBER_STRIP_COMMAND} $<TARGET_FILE:${name}>
      COMMENT "Stripping ${name}"
    )
  endif ()

  install(
    TARGETS ${name}
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/services
  )
endfunction ()
