#include <aember-libs-tests/service-manager/service.h>

namespace aember_test::service_manager {

TEST_F(ServiceTest, ConstructorInitializesCorrectly) {
  aember::service_manager::Service service(config_);

  EXPECT_EQ(service.GetName(), "test-service");
  EXPECT_EQ(service.GetState(), aember::service_manager::ServiceState::STOPPED);
  EXPECT_EQ(service.GetPid(), -1);
  EXPECT_EQ(service.GetExitCode(), 0);
  EXPECT_EQ(service.GetRestartCount(), 0);
}

TEST_F(ServiceTest, ConfigAccessor) {
  aember::service_manager::Service service(config_);

  const auto& retrieved_config = service.GetConfig();

  EXPECT_EQ(retrieved_config.name, "test-service");
  EXPECT_EQ(retrieved_config.command, "/bin/echo");
  EXPECT_EQ(retrieved_config.args.size(), 1);
  EXPECT_EQ(retrieved_config.args[0], "hello");
  EXPECT_EQ(retrieved_config.working_directory, "/tmp");
  EXPECT_EQ(retrieved_config.environment.at("TEST_VAR"), "test_value");
  EXPECT_EQ(retrieved_config.restart_policy,
            aember::service_manager::RestartPolicy::NEVER);
  EXPECT_EQ(retrieved_config.max_restart_attempts, 3);
  EXPECT_EQ(retrieved_config.restart_delay, std::chrono::seconds(1));
}

TEST_F(ServiceTest, SetAndGetState) {
  aember::service_manager::Service service(config_);

  EXPECT_EQ(service.GetState(), aember::service_manager::ServiceState::STOPPED);

  service.SetState(aember::service_manager::ServiceState::STARTING);
  EXPECT_EQ(service.GetState(),
            aember::service_manager::ServiceState::STARTING);

  service.SetState(aember::service_manager::ServiceState::RUNNING);
  EXPECT_EQ(service.GetState(), aember::service_manager::ServiceState::RUNNING);

  service.SetState(aember::service_manager::ServiceState::STOPPING);
  EXPECT_EQ(service.GetState(),
            aember::service_manager::ServiceState::STOPPING);

  service.SetState(aember::service_manager::ServiceState::STOPPED);
  EXPECT_EQ(service.GetState(), aember::service_manager::ServiceState::STOPPED);

  service.SetState(aember::service_manager::ServiceState::FAILED);
  EXPECT_EQ(service.GetState(), aember::service_manager::ServiceState::FAILED);
}

TEST_F(ServiceTest, SetAndGetPid) {
  aember::service_manager::Service service(config_);

  EXPECT_EQ(service.GetPid(), -1);

  service.SetPid(12345);
  EXPECT_EQ(service.GetPid(), 12345);

  service.SetPid(0);
  EXPECT_EQ(service.GetPid(), 0);

  service.SetPid(-1);
  EXPECT_EQ(service.GetPid(), -1);
}

TEST_F(ServiceTest, SetAndGetExitCode) {
  aember::service_manager::Service service(config_);

  EXPECT_EQ(service.GetExitCode(), 0);

  service.SetExitCode(1);
  EXPECT_EQ(service.GetExitCode(), 1);

  service.SetExitCode(127);
  EXPECT_EQ(service.GetExitCode(), 127);

  service.SetExitCode(-1);
  EXPECT_EQ(service.GetExitCode(), -1);
}

TEST_F(ServiceTest, RestartCount) {
  aember::service_manager::Service service(config_);

  EXPECT_EQ(service.GetRestartCount(), 0);

  service.IncrementRestartCount();
  EXPECT_EQ(service.GetRestartCount(), 1);

  service.IncrementRestartCount();
  EXPECT_EQ(service.GetRestartCount(), 2);

  service.IncrementRestartCount();
  EXPECT_EQ(service.GetRestartCount(), 3);

  service.ResetRestartCount();
  EXPECT_EQ(service.GetRestartCount(), 0);
}

TEST_F(ServiceTest, StartTimeTracking) {
  aember::service_manager::Service service(config_);

  auto before = std::chrono::system_clock::now();
  service.SetStartTime();
  auto after = std::chrono::system_clock::now();

  auto start_time = service.GetStartTime();

  EXPECT_GE(start_time, before);
  EXPECT_LE(start_time, after);
}

TEST_F(ServiceTest, LastRestartTimeTracking) {
  aember::service_manager::Service service(config_);

  auto before = std::chrono::system_clock::now();
  service.SetLastRestartTime();
  auto after = std::chrono::system_clock::now();

  auto restart_time = service.GetLastRestartTime();

  EXPECT_GE(restart_time, before);
  EXPECT_LE(restart_time, after);
}

TEST_F(ServiceTest, ServiceStateToString) {
  EXPECT_EQ(aember::service_manager::ServiceStateToString(
                aember::service_manager::ServiceState::STOPPED),
            "STOPPED");
  EXPECT_EQ(aember::service_manager::ServiceStateToString(
                aember::service_manager::ServiceState::STARTING),
            "STARTING");
  EXPECT_EQ(aember::service_manager::ServiceStateToString(
                aember::service_manager::ServiceState::RUNNING),
            "RUNNING");
  EXPECT_EQ(aember::service_manager::ServiceStateToString(
                aember::service_manager::ServiceState::STOPPING),
            "STOPPING");
  EXPECT_EQ(aember::service_manager::ServiceStateToString(
                aember::service_manager::ServiceState::FAILED),
            "FAILED");
}

TEST_F(ServiceTest, RestartPolicyToString) {
  EXPECT_EQ(aember::service_manager::RestartPolicyToString(
                aember::service_manager::RestartPolicy::NEVER),
            "NEVER");
  EXPECT_EQ(aember::service_manager::RestartPolicyToString(
                aember::service_manager::RestartPolicy::ON_FAILURE),
            "ON_FAILURE");
  EXPECT_EQ(aember::service_manager::RestartPolicyToString(
                aember::service_manager::RestartPolicy::ALWAYS),
            "ALWAYS");
}

TEST_F(ServiceTest, ServiceConfigConstructor) {
  aember::service_manager::ServiceConfig config1;
  EXPECT_TRUE(config1.name.empty());
  EXPECT_TRUE(config1.command.empty());

  aember::service_manager::ServiceConfig config2("my-service", "/bin/bash");
  EXPECT_EQ(config2.name, "my-service");
  EXPECT_EQ(config2.command, "/bin/bash");
}

TEST_F(ServiceTest, MultipleServicesIndependent) {
  aember::service_manager::ServiceConfig config1("service1", "/bin/echo");
  aember::service_manager::ServiceConfig config2("service2", "/bin/cat");

  aember::service_manager::Service service1(config1);
  aember::service_manager::Service service2(config2);

  service1.SetState(aember::service_manager::ServiceState::RUNNING);
  service1.SetPid(100);

  service2.SetState(aember::service_manager::ServiceState::STOPPED);
  service2.SetPid(200);

  EXPECT_EQ(service1.GetState(),
            aember::service_manager::ServiceState::RUNNING);
  EXPECT_EQ(service1.GetPid(), 100);

  EXPECT_EQ(service2.GetState(),
            aember::service_manager::ServiceState::STOPPED);
  EXPECT_EQ(service2.GetPid(), 200);
}

TEST_F(ServiceTest, ThreadSafetyStateAccess) {
  aember::service_manager::Service service(config_);

  std::atomic<bool> running{true};
  std::atomic<int> errors{0};

  // Thread that writes states
  std::thread writer([&service, &running]() {
    while (running) {
      service.SetState(aember::service_manager::ServiceState::RUNNING);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      service.SetState(aember::service_manager::ServiceState::STOPPED);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  // Thread that reads states
  std::thread reader([&service, &running, &errors]() {
    while (running) {
      auto state = service.GetState();
      // Just verify we can read without crashing
      if (state != aember::service_manager::ServiceState::RUNNING &&
          state != aember::service_manager::ServiceState::STOPPED) {
        errors++;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  running = false;

  writer.join();
  reader.join();

  EXPECT_EQ(errors, 0);
}

TEST_F(ServiceTest, ThreadSafetyPidAccess) {
  aember::service_manager::Service service(config_);

  std::atomic<bool> running{true};

  // Thread that writes PIDs
  std::thread writer([&service, &running]() {
    pid_t pid = 1000;
    while (running) {
      service.SetPid(pid++);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  // Thread that reads PIDs
  std::thread reader([&service, &running]() {
    while (running) {
      auto pid = service.GetPid();
      // Just verify we can read without crashing
      (void)pid;
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  running = false;

  writer.join();
  reader.join();
}

}  // namespace aember_test::service_manager
