#include <chrono>

#include <aember-libs-tests/service-manager/service-manager.h>

namespace aember_test::service_manager {

TEST_F(ServiceManagerTest, ConstructorInitializes) {
  EXPECT_NE(manager_, nullptr);
}

TEST_F(ServiceManagerTest, AddServiceBasic) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->HasService("test-service"));
}

TEST_F(ServiceManagerTest, AddServiceWithEmptyName) {
  auto config = CreateTestConfig("", "/bin/echo");

  EXPECT_FALSE(manager_->AddService(config));
}

TEST_F(ServiceManagerTest, AddServiceWithEmptyCommand) {
  auto config = CreateTestConfig("test-service", "");

  EXPECT_FALSE(manager_->AddService(config));
}

TEST_F(ServiceManagerTest, AddDuplicateService) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_FALSE(manager_->AddService(config));  // Second add should fail
}

TEST_F(ServiceManagerTest, RemoveService) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->HasService("test-service"));

  EXPECT_TRUE(manager_->RemoveService("test-service"));
  EXPECT_FALSE(manager_->HasService("test-service"));
}

TEST_F(ServiceManagerTest, RemoveNonExistentService) {
  EXPECT_FALSE(manager_->RemoveService("non-existent"));
}

TEST_F(ServiceManagerTest, RemoveRunningService) {
  auto config = CreateTestConfig("test-service", "/bin/sleep");
  config.args = {"10"};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  EXPECT_TRUE(WaitForServiceState(
      "test-service", aember::service_manager::ServiceState::RUNNING));

  EXPECT_FALSE(manager_->RemoveService("test-service"));

  // Cleanup
  manager_->StopService("test-service");
}

TEST_F(ServiceManagerTest, StartServiceBasic) {
  auto config = CreateTestConfig("test-service", "/bin/true");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  // /bin/true exits immediately, so we need to handle its exit
  auto service = manager_->GetService("test-service");
  pid_t pid = service->GetPid();

  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);  // Reap the process
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
    manager_->HandleServiceExit(pid, exit_code);
  }

  // Give it time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Service should have exited cleanly
  EXPECT_EQ(service->GetState(),
            aember::service_manager::ServiceState::STOPPED);
}

TEST_F(ServiceManagerTest, StartNonExistentService) {
  EXPECT_FALSE(manager_->StartService("non-existent"));
}

TEST_F(ServiceManagerTest, StartServiceTwice) {
  auto config = CreateTestConfig("test-service", "/bin/sleep");
  config.args = {"1"};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));
  EXPECT_TRUE(WaitForServiceState(
      "test-service", aember::service_manager::ServiceState::RUNNING));

  // Starting again should succeed (idempotent)
  EXPECT_TRUE(manager_->StartService("test-service"));

  // Cleanup
  manager_->StopService("test-service");
}

TEST_F(ServiceManagerTest, StopServiceBasic) {
  auto config = CreateTestConfig("test-service", "/bin/sleep");
  config.args = {"10"};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));
  EXPECT_TRUE(WaitForServiceState(
      "test-service", aember::service_manager::ServiceState::RUNNING));

  auto service = manager_->GetService("test-service");
  pid_t pid = service->GetPid();

  EXPECT_TRUE(manager_->StopService("test-service"));

  // Manually reap the process and notify service manager
  int status;
  waitpid(pid, &status, 0);
  int exit_code =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  manager_->HandleServiceExit(pid, exit_code);

  EXPECT_TRUE(
      WaitForServiceState("test-service",
                          aember::service_manager::ServiceState::STOPPED,
                          std::chrono::seconds(2)));
}

TEST_F(ServiceManagerTest, StopNonExistentService) {
  EXPECT_FALSE(manager_->StopService("non-existent"));
}

TEST_F(ServiceManagerTest, StopAlreadyStoppedService) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(
      manager_->StopService("test-service"));  // Should succeed (idempotent)
}

TEST_F(ServiceManagerTest, RestartService) {
  auto config = CreateTestConfig("test-service", "/bin/sleep");
  config.args = {"1"};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));
  EXPECT_TRUE(WaitForServiceState(
      "test-service", aember::service_manager::ServiceState::RUNNING));

  auto service = manager_->GetService("test-service");
  pid_t first_pid = service->GetPid();

  EXPECT_TRUE(manager_->RestartService("test-service"));
  EXPECT_TRUE(WaitForServiceState(
      "test-service", aember::service_manager::ServiceState::RUNNING));

  // Should have a new PID after restart
  pid_t second_pid = service->GetPid();
  EXPECT_NE(first_pid, second_pid);

  // Cleanup
  manager_->StopService("test-service");
}

TEST_F(ServiceManagerTest, GetServiceState) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  // Before adding
  EXPECT_EQ(manager_->GetServiceState("test-service"),
            aember::service_manager::ServiceState::STOPPED);

  // After adding
  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_EQ(manager_->GetServiceState("test-service"),
            aember::service_manager::ServiceState::STOPPED);
}

TEST_F(ServiceManagerTest, GetServiceNames) {
  EXPECT_TRUE(manager_->GetServiceNames().empty());

  auto config1 = CreateTestConfig("service1", "/bin/echo");
  auto config2 = CreateTestConfig("service2", "/bin/cat");
  auto config3 = CreateTestConfig("service3", "/bin/ls");

  manager_->AddService(config1);
  manager_->AddService(config2);
  manager_->AddService(config3);

  auto names = manager_->GetServiceNames();
  EXPECT_EQ(names.size(), 3);

  EXPECT_NE(std::find(names.begin(), names.end(), "service1"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "service2"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "service3"), names.end());
}

TEST_F(ServiceManagerTest, GetService) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  EXPECT_EQ(manager_->GetService("test-service"), nullptr);

  EXPECT_TRUE(manager_->AddService(config));

  auto service = manager_->GetService("test-service");
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(service->GetName(), "test-service");
}

TEST_F(ServiceManagerTest, HasService) {
  auto config = CreateTestConfig("test-service", "/bin/echo");

  EXPECT_FALSE(manager_->HasService("test-service"));

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->HasService("test-service"));

  EXPECT_TRUE(manager_->RemoveService("test-service"));
  EXPECT_FALSE(manager_->HasService("test-service"));
}

TEST_F(ServiceManagerTest, StateChangeCallback) {
  TrackStateChanges();

  auto config = CreateTestConfig("test-service", "/bin/sleep");
  config.args = {"1"};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  EXPECT_TRUE(WaitForServiceState(
      "test-service", aember::service_manager::ServiceState::RUNNING));

  // Should have received state change notifications
  EXPECT_GE(state_changes_.size(), 2);  // At least STOPPED->STARTING->RUNNING

  // Cleanup
  manager_->StopService("test-service");
}

TEST_F(ServiceManagerTest, ServiceWithArguments) {
  auto config = CreateTestConfig("test-service", "/bin/echo");
  config.args = {"hello", "world"};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(ServiceManagerTest, ServiceWithEnvironment) {
  auto config = CreateTestConfig("test-service", "/bin/sh");
  config.args = {"-c", "exit 0"};
  config.environment = {{"TEST_VAR1", "value1"}, {"TEST_VAR2", "value2"}};

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(ServiceManagerTest, ServiceWithWorkingDirectory) {
  auto config = CreateTestConfig("test-service", "/bin/pwd");
  config.working_directory = "/tmp";

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(ServiceManagerTest, HandleServiceExitSuccess) {
  auto config = CreateTestConfig("test-service", "/bin/true");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  auto service = manager_->GetService("test-service");
  pid_t pid = service->GetPid();

  // Wait for process to exit
  int status;
  waitpid(pid, &status, 0);

  // Notify service manager
  manager_->HandleServiceExit(pid, WEXITSTATUS(status));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(service->GetState(),
            aember::service_manager::ServiceState::STOPPED);
  EXPECT_EQ(service->GetExitCode(), 0);
}

TEST_F(ServiceManagerTest, HandleServiceExitFailure) {
  auto config = CreateTestConfig("test-service", "/bin/false");

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  auto service = manager_->GetService("test-service");
  pid_t pid = service->GetPid();

  // Wait for process to exit
  int status;
  waitpid(pid, &status, 0);

  // Notify service manager
  manager_->HandleServiceExit(pid, WEXITSTATUS(status));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(service->GetState(), aember::service_manager::ServiceState::FAILED);
  EXPECT_NE(service->GetExitCode(), 0);
}

TEST_F(ServiceManagerTest, RestartPolicyNever) {
  auto config = CreateTestConfig("test-service", "/bin/false");
  config.restart_policy = aember::service_manager::RestartPolicy::NEVER;

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  auto service = manager_->GetService("test-service");
  pid_t pid = service->GetPid();

  // Wait for process to exit and reap it
  int status;
  ASSERT_GT(pid, 0);
  ASSERT_EQ(waitpid(pid, &status, 0), pid);

  // Notify service manager
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  manager_->HandleServiceExit(pid, exit_code);

  // Give it a moment to process
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should not restart
  EXPECT_EQ(service->GetRestartCount(), 0);
  EXPECT_EQ(service->GetState(), aember::service_manager::ServiceState::FAILED);
}

TEST_F(ServiceManagerTest, RestartPolicyOnFailure) {
  auto config = CreateTestConfig("test-service", "/bin/false");
  config.restart_policy = aember::service_manager::RestartPolicy::ON_FAILURE;
  config.max_restart_attempts = 2;
  config.restart_delay = std::chrono::seconds(1);  // Shorter delay for testing

  TrackStateChanges();

  EXPECT_TRUE(manager_->AddService(config));
  EXPECT_TRUE(manager_->StartService("test-service"));

  auto service = manager_->GetService("test-service");

  // Manually handle exits to control the test
  for (int i = 0; i < 3; ++i) {  // Initial start + 2 restarts
    pid_t pid = service->GetPid();
    if (pid <= 0) break;

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    manager_->HandleServiceExit(pid, exit_code);

    // Wait for restart delay + processing time
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }

  // Should have attempted restarts
  EXPECT_EQ(service->GetRestartCount(), 2);
  EXPECT_EQ(service->GetState(), aember::service_manager::ServiceState::FAILED);
}

TEST_F(ServiceManagerTest, StartAll) {
  auto config1 = CreateTestConfig("service1", "/bin/sleep");
  config1.args = {"1"};
  auto config2 = CreateTestConfig("service2", "/bin/sleep");
  config2.args = {"1"};
  auto config3 = CreateTestConfig("service3", "/bin/sleep");
  config3.args = {"1"};

  manager_->AddService(config1);
  manager_->AddService(config2);
  manager_->AddService(config3);

  manager_->StartAll();

  EXPECT_TRUE(WaitForServiceState(
      "service1", aember::service_manager::ServiceState::RUNNING));
  EXPECT_TRUE(WaitForServiceState(
      "service2", aember::service_manager::ServiceState::RUNNING));
  EXPECT_TRUE(WaitForServiceState(
      "service3", aember::service_manager::ServiceState::RUNNING));

  // Cleanup
  manager_->StopAll();
}

TEST_F(ServiceManagerTest, StopAll) {
  auto config1 = CreateTestConfig("service1", "/bin/sleep");
  config1.args = {"10"};
  auto config2 = CreateTestConfig("service2", "/bin/sleep");
  config2.args = {"10"};

  manager_->AddService(config1);
  manager_->AddService(config2);

  manager_->StartService("service1");
  manager_->StartService("service2");

  EXPECT_TRUE(WaitForServiceState(
      "service1", aember::service_manager::ServiceState::RUNNING));
  EXPECT_TRUE(WaitForServiceState(
      "service2", aember::service_manager::ServiceState::RUNNING));

  manager_->StopAll();

  EXPECT_TRUE(
      WaitForServiceState("service1",
                          aember::service_manager::ServiceState::STOPPED,
                          std::chrono::seconds(5)));
  EXPECT_TRUE(
      WaitForServiceState("service2",
                          aember::service_manager::ServiceState::STOPPED,
                          std::chrono::seconds(5)));
}

TEST_F(ServiceManagerTest, ServiceDependenciesBasic) {
  auto config1 = CreateTestConfig("service1", "/bin/sleep");
  config1.args = {"10"};

  auto config2 = CreateTestConfig("service2", "/bin/sleep");
  config2.args = {"10"};
  config2.dependencies = {"service1"};

  manager_->AddService(config1);
  manager_->AddService(config2);

  // Starting service2 should also start service1
  EXPECT_TRUE(manager_->StartService("service2"));

  EXPECT_TRUE(WaitForServiceState(
      "service1", aember::service_manager::ServiceState::RUNNING));
  EXPECT_TRUE(WaitForServiceState(
      "service2", aember::service_manager::ServiceState::RUNNING));

  // Stop all services
  manager_->StopAll();

  // Reap all running services
  ReapAllRunningServices();

  // Verify they stopped
  EXPECT_TRUE(
      WaitForServiceState("service1",
                          aember::service_manager::ServiceState::STOPPED,
                          std::chrono::seconds(1)));
  EXPECT_TRUE(
      WaitForServiceState("service2",
                          aember::service_manager::ServiceState::STOPPED,
                          std::chrono::seconds(1)));
}

TEST_F(ServiceManagerTest, ServiceDependenciesMissing) {
  auto config = CreateTestConfig("service1", "/bin/sleep");
  config.args = {"10"};
  config.dependencies = {"non-existent"};

  manager_->AddService(config);

  // Should fail to start due to missing dependency
  EXPECT_FALSE(manager_->StartService("service1"));
}

TEST_F(ServiceManagerTest, MultipleServicesIndependent) {
  auto config1 = CreateTestConfig("service1", "/bin/sleep");
  config1.args = {"1"};

  auto config2 = CreateTestConfig("service2", "/bin/sleep");
  config2.args = {"1"};

  manager_->AddService(config1);
  manager_->AddService(config2);

  manager_->StartService("service1");
  manager_->StartService("service2");

  auto service1 = manager_->GetService("service1");
  auto service2 = manager_->GetService("service2");

  EXPECT_NE(service1->GetPid(), service2->GetPid());

  pid_t pid1 = service1->GetPid();
  pid_t pid2 = service2->GetPid();

  // Cleanup
  manager_->StopAll();

  // Manually reap processes
  int status;
  waitpid(pid1, &status, 0);
  int exit_code1 =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  manager_->HandleServiceExit(pid1, exit_code1);

  waitpid(pid2, &status, 0);
  int exit_code2 =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  manager_->HandleServiceExit(pid2, exit_code2);
}

}  // namespace aember_test::service_manager
