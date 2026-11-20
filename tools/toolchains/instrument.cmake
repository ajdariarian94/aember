# ---------------------------------------------------------------------------
# Instrumented builds for the tests (`aember test --coverage`,
# `aember test --sanitize asan|tsan`).
#
# AEMBER_INSTRUMENT (passed by ConfigureStep, forwarded by the superbuild to
# every sub-project):
#   coverage  gcov counters (-O0, atomic updates: the tests are threaded)
#   asan      AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer
#   tsan      ThreadSanitizer
#   (empty)   a normal build
#
# vcpkg chain-loads this toolchain for its port builds too, but without
# AEMBER_INSTRUMENT, so dependencies (gtest, spdlog, fmt, ...) stay plain.
# The sanitizer runtimes come from the SDK's target sysroot
# (aember-resources v0.1.8+, gcc-sanitizers).
# ---------------------------------------------------------------------------
if(NOT DEFINED AEMBER_INSTRUMENT OR AEMBER_INSTRUMENT STREQUAL "")
  return()
endif()

set(_aember_debug "-g -fno-omit-frame-pointer")
if(AEMBER_INSTRUMENT STREQUAL "coverage")
  # AEMBER_COVERAGE: tests flush the counters of forked children with
  # __gcov_dump() before _exit() (they'd be lost otherwise).
  set(_aember_compile "-O0 -g --coverage -fprofile-update=atomic -DAEMBER_COVERAGE")
  set(_aember_link "--coverage")
elseif(AEMBER_INSTRUMENT STREQUAL "asan")
  set(_aember_compile
      "-O1 ${_aember_debug} -fsanitize=address,undefined -fno-sanitize-recover=undefined"
  )
  set(_aember_link "-fsanitize=address,undefined")
elseif(AEMBER_INSTRUMENT STREQUAL "tsan")
  set(_aember_compile "-O1 ${_aember_debug} -fsanitize=thread")
  set(_aember_link "-fsanitize=thread")
else()
  message(FATAL_ERROR
    "AEMBER_INSTRUMENT='${AEMBER_INSTRUMENT}': expected coverage, asan or tsan")
endif()

string(APPEND CMAKE_C_FLAGS_INIT " ${_aember_compile}")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${_aember_compile}")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${_aember_link}")
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${_aember_link}")
