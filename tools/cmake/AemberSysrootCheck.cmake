# -----------------------------------------------------------------------------
# A build directory whose compiler probe belongs to another SDK sysroot
#
# CMake probes the compiler once per build directory (CMakeFiles/<v>/
# CMakeCXXCompiler.cmake) and keeps its implicit include directories.
# After an aember-resources SDK changes its target sysroot (aarch64:
# cortexa72 -> cortexa53), an old build directory still lists the old one,
# so <sysroot>/usr/include is no longer "implicit": a package that reports it
# (FindLibLZMA) is added as -isystem, which breaks libstdc++'s
# #include_next <stdlib.h> ("stdlib.h: No such file or directory").
# aember's CLI removes such a directory before configuring
# (steps/cmake/configure.py); this stops a direct cmake run with the reason.
# -----------------------------------------------------------------------------

if (CMAKE_SYSROOT AND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
  file(
    REAL_PATH
    "${CMAKE_SYSROOT}/usr/include"
    _aember_sysroot_include
  )
  set(_aember_implicit "")
  foreach (_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    file(
      REAL_PATH
      "${_dir}"
      _real
    )
    list(
      APPEND
      _aember_implicit
      "${_real}"
    )
  endforeach ()
  if (NOT
      _aember_sysroot_include
      IN_LIST
      _aember_implicit
  )
    message(
      FATAL_ERROR
        "This build directory's compiler probe doesn't include the SDK "
        "sysroot ${CMAKE_SYSROOT}/usr/include (it was made with another SDK). "
        "Delete ${CMAKE_BINARY_DIR}'s top-level configure directory "
        "(build/configure/<arch>/<type>/) and configure again; `aember app "
        "build` does that by itself."
    )
  endif ()
endif ()
