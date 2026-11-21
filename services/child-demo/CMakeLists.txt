# aember-child-demo: the smallest supervised service. It logs a line every 10 s
# and exits cleanly on SIGTERM/SIGINT, so PID 1's restart and stop handling can
# be watched (the website's kill-service demo kills it).

find_package(
  aember-libs
  REQUIRED
  COMPONENTS utils
)

aember_service(
  aember-child-demo
  MAIN
  src/main.cpp
  LIBRARIES
  aember-libs::utils::logging
)
