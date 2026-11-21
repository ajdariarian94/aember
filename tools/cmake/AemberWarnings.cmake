# Compiler warnings for aember's own code (libraries, services, executables
# and their tests). Included right after project() by each of those
# sub-projects; vcpkg's dependencies never see these flags.
#
# AEMBER_WERROR (default ON) turns them into errors: the code is kept
# warning-free, `aember analyze` and CI rely on it. -DAEMBER_WERROR=OFF to
# experiment locally.
#
# Not used: -Wnull-dereference (depends on the optimizer; false positives in
# libstdc++'s own headers at -O2).
option(AEMBER_WERROR "Treat aember's compiler warnings as errors" ON)

add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wcast-align
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
  -Wmisleading-indentation
  -Wduplicated-cond
  -Wduplicated-branches
  -Wlogical-op
  -Wsuggest-override
)
if(AEMBER_WERROR)
  add_compile_options(-Werror)
endif()
