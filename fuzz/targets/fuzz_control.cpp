/**
 * @file fuzz_control.cpp
 * @brief aember-containerd's control socket: any request bytes
 *        (AnswerControlRequest), against a fake set of containers.
 *        Properties: never crashes or throws; the answer is a JSON object
 *        with a boolean "ok"; nothing outside the known containers is
 *        stopped or started.
 */
#include <aember-services/containerd/control.h>

#include <map>

#include "common.h"

namespace {

struct Target final : aember::containerd::ControlTarget {
  std::map<std::string, std::string, std::less<>> states{{"web", "running"},
                                                         {"db", "stopped"}};
  [[nodiscard]] std::vector<Entry> List() const override {
    std::vector<Entry> list;
    for (const auto& [name, state] : states) list.push_back({name, state});
    return list;
  }
  [[nodiscard]] bool Has(std::string_view name) const override {
    return states.contains(name);
  }
  bool Stop(std::string_view name) override { return Set(name, "stopped"); }
  bool Start(std::string_view name) override { return Set(name, "running"); }
  bool Set(std::string_view name, const char* state) {
    const auto it = states.find(name);
    if (it == states.end()) fuzz::Fail("acted on an unknown container");
    it->second = state;
    return true;
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  Target target;
  const auto answer = nlohmann::json::parse(
      aember::containerd::AnswerControlRequest(fuzz::Text(data, size), target),
      nullptr,
      false);
  if (!answer.is_object()) fuzz::Fail("answer isn't a JSON object");
  const auto ok = answer.find("ok");
  if (ok == answer.end() || !ok->is_boolean()) fuzz::Fail("no boolean ok");
  if (target.states.size() != 2) fuzz::Fail("containers added");
  return 0;
}
