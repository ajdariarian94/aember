/**
 * @file init-wrapper.cpp
 * @author Arian Ajdari
 * @brief The container init wrapper script, run with the host's sh: the
 *        app's arguments, exit status, signal forwarding, and the console
 *        redirect only inside a container.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-services/containerd/init-wrapper.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using aember::containerd::kInitScript;

struct Run {
  int status{-1};
  std::string out;
};

/// sh -c kInitScript aember-init <app...>, stdout captured; @p on_start
/// gets the wrapper's pid once it runs.
Run RunWrapper(const std::vector<std::string>& app,
               const std::vector<std::string>& env = {},
               void (*on_start)(pid_t) = nullptr) {
  std::array<int, 2> fds{};
  EXPECT_EQ(::pipe(fds.data()), 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(fds[1], 1);
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    ::close(fds[1]);
    std::vector<std::string> args{"sh", "-c", kInitScript, "aember-init"};
    args.insert(args.end(), app.begin(), app.end());
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> envs = env;
    envs.emplace_back("PATH=/usr/bin:/bin");
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    ::execve("/bin/sh", argv.data(), envp.data());
    ::_exit(127);
  }
  ::close(fds[1]);
  if (on_start) on_start(pid);
  Run r;
  std::array<char, 256> buf{};
  ssize_t n = 0;
  while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
    r.out.append(buf.data(), n);
  ::close(fds[0]);
  ::waitpid(pid, &r.status, 0);
  return r;
}

TEST(InitWrapper, RunsTheAppWithItsArguments) {
  const auto r = RunWrapper({"/bin/sh", "-c", "echo \"$0 $1\"", "a b", "c"});
  ASSERT_TRUE(WIFEXITED(r.status));
  EXPECT_EQ(WEXITSTATUS(r.status), 0);
  EXPECT_EQ(r.out, "a b c\n");
}

TEST(InitWrapper, ExitsWithTheAppsStatus) {
  const auto r = RunWrapper({"/bin/sh", "-c", "exit 7"});
  ASSERT_TRUE(WIFEXITED(r.status));
  EXPECT_EQ(WEXITSTATUS(r.status), 7);
}

// Regression: an app that is the container's init itself never got
// SIGTERM, so every stop ran into its timeout.
TEST(InitWrapper, PassesSigtermToTheApp) {
  const auto r =
      RunWrapper({"/bin/sh",
                  "-c",
                  "trap 'echo got-term; exit 3' TERM; echo up; while :; do "
                  "sleep 0.05; done"},
                 {},
                 [](pid_t pid) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(400));
                   ::kill(pid, SIGTERM);
                 });
  ASSERT_TRUE(WIFEXITED(r.status));
  EXPECT_EQ(WEXITSTATUS(r.status), 3);
  EXPECT_NE(r.out.find("got-term"), std::string::npos) << r.out;
}

TEST(InitWrapper, OutsideAContainerTheOutputStays) {
  // container=lxc unset: no redirect, even where /dev/console is writable.
  const auto r = RunWrapper({"/bin/echo", "here"});
  EXPECT_EQ(r.out, "here\n");
}

TEST(InitWrapper, WithoutAConsoleTheAppStillRuns) {
  // In a container whose /dev/console is missing or can't be opened,
  // nothing changes, and no /dev/console file is created (as root).
  struct stat st {};
  const bool had_console = ::stat("/dev/console", &st) == 0;
  if (had_console && S_ISCHR(st.st_mode) && ::access("/dev/console", W_OK) == 0)
    GTEST_SKIP() << "a writable console here";
  const auto r = RunWrapper({"/bin/echo", "here"}, {"container=lxc"});
  ASSERT_TRUE(WIFEXITED(r.status));
  EXPECT_EQ(WEXITSTATUS(r.status), 0);
  EXPECT_EQ(r.out, "here\n");
  EXPECT_EQ(::stat("/dev/console", &st) == 0, had_console);
}

}  // namespace
