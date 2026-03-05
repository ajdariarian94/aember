# -----------------------------------------------------------------------------
# Logging subcomponent
# -----------------------------------------------------------------------------

add_library(${LOGGING_TARGET} STATIC src/logging.cpp)

add_library(${PROJECT_NAME}::${COMPONENT}::${LOGGING_COMPONENT} ALIAS
            ${LOGGING_TARGET})

target_include_directories(
  ${LOGGING_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(${LOGGING_TARGET} PUBLIC spdlog::spdlog)

set_target_properties(${LOGGING_TARGET}
                      PROPERTIES EXPORT_NAME ${COMPONENT}::${LOGGING_COMPONENT})

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if(BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(tests)
endif()
