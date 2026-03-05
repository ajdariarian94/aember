# -----------------------------------------------------------------------------
# Signal subcomponent
# -----------------------------------------------------------------------------

add_library(${SIGNAL_TARGET} STATIC src/signal.cpp)

add_library(${PROJECT_NAME}::${COMPONENT}::${SIGNAL_COMPONENT} ALIAS
            ${SIGNAL_TARGET})

target_include_directories(
  ${SIGNAL_TARGET} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(${SIGNAL_TARGET}
                      PROPERTIES EXPORT_NAME ${COMPONENT}::${SIGNAL_COMPONENT})
