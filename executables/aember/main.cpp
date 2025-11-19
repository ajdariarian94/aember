#include <aember-libs/device-health/heartbeat.h>
#include <aember-libs/utils/logging/logging.h>


aember::utils::Logger log_{"aember"};

void HeartbeatCallback(const nlohmann::json& heartbeat) {
  log_.info(heartbeat.dump());
}

int main(int /*argc*/, char** /*argv*/) {
  aember::device_health::Heartbeat device_health{HeartbeatCallback};

  device_health.Start();

  while (true) { std::this_thread::sleep_for(std::chrono::seconds(1)); }

  return 0;
}
