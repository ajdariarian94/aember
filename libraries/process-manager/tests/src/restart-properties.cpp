/**
 * @file restart-properties.cpp
 * @author Arian Ajdari
 * @brief Property test: ProcessManager's restart decisions against a
 *        reference model, on random sequences of short and long runs that
 *        exit cleanly or not. The model (also in spec/tla/StartGraph.tla,
 *        ResetOnStable): every unrequested exit asks for a restart; a run of
 *        at least stable_run starts the count over; with max_restarts > 0,
 *        the exit that finds the count at max_restarts is given up (Failed).
 *
 * Runs real processes: short ones exit at once, long ones sleep 2x
 * stable_run, so the classification has plenty of margin on a loaded CI
 * machine.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/process-manager/process-manager.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include <aember-test/property.h>

namespace {

using aember::process_manager::ProcessManager;
using aember::test::Gen;
using State = ProcessManager::ProcessState;
using namespace std::chrono_literals;

TEST(RestartProperty, MatchesTheModel) {
  aember::test::ForAll(6, [](Gen& g) {
    // One registration (its restart count is the state under test); each
    // run reads how long to sleep and its exit code from a file.
    const std::string plan = ::testing::TempDir() + "aember-restart-plan-" +
                             std::to_string(::getpid());
    ProcessManager pm;
    pm.SetRestartCallback([](const std::string&) {});  // restart by hand
    ProcessManager::ProcessConfig cfg;
    cfg.name = "p";
    cfg.executable = "/bin/sh";
    cfg.args = {"-c", "read s c < '" + plan + "'; sleep $s; exit $c"};
    cfg.restart_on_failure = true;
    cfg.max_restarts = g.Int(0u, 3u);
    cfg.restart_delay = 1ms;
    cfg.stable_run = 200ms;
    ASSERT_TRUE(pm.AddProcess(cfg));

    unsigned count = 0;  // the model's restart count
    for (int run = g.Int(1, 5); run > 0; --run) {
      const bool long_run = g.Chance(0.4);
      const int code = g.Chance(0.3) ? 0 : 3;
      std::ofstream{plan} << (long_run ? "0.4" : "0") << ' ' << code << '\n';
      const auto pid = pm.Start("p");
      ASSERT_TRUE(pid);
      int status = 0;
      ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
      ASSERT_TRUE(pm.HandleExit(*pid, status));

      if (long_run) count = 0;  // ran fine for a while
      State want = State::Restarting;
      if (cfg.max_restarts > 0 && count >= cfg.max_restarts) {
        want = State::Failed;  // given up
      } else {
        ++count;
      }
      EXPECT_EQ(pm.GetState("p"), want)
          << "max_restarts " << cfg.max_restarts << ", "
          << (long_run ? "long" : "short") << " run, exit " << code;
    }
    std::filesystem::remove(plan);
  });
}

}  // namespace
