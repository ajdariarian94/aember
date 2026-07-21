#include <aember-libs/device-health/aember-monitor.h>
#include <aember-libs/utils/logging/logging.h>

#include <meta>
#include <string_view>
#include <thread>

using SelfStats = aember::device_health::AemberMonitorClient::SelfStats;

// ---------------------------------------------------------------------------
// C++26 P2996 reflection
//
// Mirror of the json_to_object pattern from the working example:
//   - consteval function uses std::vector<std::meta::info> locally
//   - generates a struct type via substitute + Cls
//   - each field holds its own name as const char*
//   - splice at global scope produces a constexpr struct instance
//
// Result: kFields.pid == "pid", kFields.ppid == "ppid", etc.
// Field names come from SelfStats definition — add a field there and
// it appears here automatically at compile time.
// ---------------------------------------------------------------------------

template <std::meta::info... Ms>
struct Outer {
  struct Inner;
  consteval { define_aggregate(^^Inner, {Ms...}); }
};

template <std::meta::info... Ms>
using Cls = Outer<Ms...>::Inner;

template <typename T, auto... Vs>
constexpr T construct_from{Vs...};

constexpr std::string_view AemberMonitorClientName =
    std::meta::identifier_of(^^aember::device_health::AemberMonitorClient);

consteval std::meta::info get_field_names_info() {
  std::vector<std::meta::info> ms;
  std::vector<std::meta::info> vs = {^^void};

  for (auto m : std::meta::nonstatic_data_members_of(
           ^^SelfStats, std::meta::access_context::current())) {
    auto dms = data_member_spec(
        ^^char const*, {.name = std::string(std::meta::identifier_of(m))});
    ms.push_back(std::meta::reflect_constant(dms));
    vs.push_back(
        std::meta::reflect_constant_string(std::meta::identifier_of(m)));
  }

  vs[0] = std::meta::substitute(^^Cls, ms);
  return std::meta::substitute(^^construct_from, vs);
}

// Splice the generated struct — each field holds its own name as const char*.
constexpr auto SelfStatsFields = [:get_field_names_info():];

int main() {
  aember::utils::logging::Logger log_{"aember-child-demo"};

  aember::utils::logging::enable_console_silence();
  aember::utils::logging::enable_file_logging("/var/log/aember-child-demo.log");

  aember::device_health::AemberMonitorClient monitor{"aember-child-demo"};

  // C++26 P2996: type name reflected at compile time.
  log_.info("Monitor type: {} (C++26 P2996 reflection)",
            AemberMonitorClientName);

  // C++26 P2996: field names generated from SelfStats at compile time.
  // SelfStatsFields is a generated struct — each member holds its own name.
  log_.info("Monitored fields (reflected at compile time):");
  log_.info("  ├─ {}", SelfStatsFields.pid);
  log_.info("  ├─ {}", SelfStatsFields.ppid);
  log_.info("  ├─ {}", SelfStatsFields.vm_rss_kb);
  log_.info("  ├─ {}", SelfStatsFields.uptime_s);
  log_.info("  └─ {}", SelfStatsFields.pid1_rss_kb);

  monitor.Report();

  while (true) {
    std::this_thread::sleep_for(std::chrono::minutes(1));
    monitor.Report();
  }

  return 0;
}
