#include <aember-libs/device_health/heartbeat.h>

#include <spdlog/spdlog.h>

void HeartbeatCallback(const nlohmann::json& heartbeat) {
  spdlog::info(heartbeat.dump());
}

int main(int /*argc*/, char** /*argv*/) {
  aember::device_health::Heartbeat device_health{HeartbeatCallback};

  device_health.Start();

  while (true) { std::this_thread::sleep_for(std::chrono::seconds(1)); }

  return 0;
}
