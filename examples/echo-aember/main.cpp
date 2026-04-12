#include <aember-libs/utils/logging/logging.h>

#include <chrono>
#include <thread>

int main() {
  aember::utils::logging::Logger log_{"echo-aember"};

  aember::utils::logging::enable_file_logging("/var/log/echo-aember.log");

  while (true) {
    log_.info("Echo Aember example running");
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  return 0;
}
