#include <aember-libs-tests/config-manager/config-manager.h>

namespace aember_test::config_manager {

TEST_F(ConfigManagerTest, ConstructorInitializes) {
  EXPECT_NE(manager_, nullptr);
  EXPECT_FALSE(manager_->IsLoaded());
}

TEST_F(ConfigManagerTest, LoadFromStringBasic) {
  std::string config = GetBasicValidConfig();

  EXPECT_TRUE(manager_->LoadFromString(config));
  EXPECT_TRUE(manager_->IsLoaded());

  auto services = manager_->GetServices();
  EXPECT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].name, "test-service");
  EXPECT_EQ(services[0].command, "/bin/echo");
}

TEST_F(ConfigManagerTest, LoadFromFileBasic) {
  std::string config = GetBasicValidConfig();
  std::string path = CreateTestConfigFile("config.json", config);

  EXPECT_TRUE(manager_->LoadFromFile(path));
  EXPECT_TRUE(manager_->IsLoaded());

  auto services = manager_->GetServices();
  EXPECT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].name, "test-service");
  EXPECT_EQ(services[0].command, "/bin/echo");
}

TEST_F(ConfigManagerTest, LoadFromFileNonExistent) {
  EXPECT_FALSE(manager_->LoadFromFile("/non/existent/file.json"));
  EXPECT_FALSE(manager_->IsLoaded());

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("Failed to open"), std::string::npos);
}

TEST_F(ConfigManagerTest, LoadFromStringInvalidJson) {
  std::string invalid_json = "{ invalid json }";

  EXPECT_FALSE(manager_->LoadFromString(invalid_json));
  EXPECT_FALSE(manager_->IsLoaded());

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("parse error"), std::string::npos);
}

TEST_F(ConfigManagerTest, LoadFromStringNotAnObject) {
  std::string config = R"(["array", "not", "object"])";

  EXPECT_FALSE(manager_->LoadFromString(config));
  EXPECT_FALSE(manager_->IsLoaded());

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("must be a JSON object"), std::string::npos);
}

TEST_F(ConfigManagerTest, LoadFullConfigAllFields) {
  std::string config = GetFullConfig();

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);

  const auto& service = services[0];
  EXPECT_EQ(service.name, "full-service");
  EXPECT_EQ(service.command, "/usr/bin/myapp");

  ASSERT_EQ(service.args.size(), 3);
  EXPECT_EQ(service.args[0], "--port");
  EXPECT_EQ(service.args[1], "8080");
  EXPECT_EQ(service.args[2], "--verbose");

  EXPECT_EQ(service.working_directory, "/var/app");

  ASSERT_EQ(service.environment.size(), 2);
  EXPECT_EQ(service.environment.at("LOG_LEVEL"), "debug");
  EXPECT_EQ(service.environment.at("API_KEY"), "secret123");

  EXPECT_EQ(service.restart_policy,
            aember::service_manager::RestartPolicy::ALWAYS);
  EXPECT_EQ(service.max_restart_attempts, 10);
  EXPECT_EQ(service.restart_delay, std::chrono::seconds(5));

  ASSERT_EQ(service.dependencies.size(), 2);
  EXPECT_EQ(service.dependencies[0], "network");
  EXPECT_EQ(service.dependencies[1], "database");
}

TEST_F(ConfigManagerTest, LoadMultipleServices) {
  std::string config = GetMultipleServicesConfig();

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  EXPECT_EQ(services.size(), 3);

  EXPECT_EQ(services[0].name, "service1");
  EXPECT_EQ(services[1].name, "service2");
  EXPECT_EQ(services[2].name, "service3");

  EXPECT_EQ(services[1].dependencies.size(), 1);
  EXPECT_EQ(services[1].dependencies[0], "service1");

  EXPECT_EQ(services[2].restart_policy,
            aember::service_manager::RestartPolicy::ON_FAILURE);
}

TEST_F(ConfigManagerTest, GetServiceByName) {
  std::string config = GetMultipleServicesConfig();
  EXPECT_TRUE(manager_->LoadFromString(config));

  auto service1 = manager_->GetService("service1");
  ASSERT_TRUE(service1.has_value());
  EXPECT_EQ(service1->name, "service1");
  EXPECT_EQ(service1->command, "/bin/service1");

  auto service2 = manager_->GetService("service2");
  ASSERT_TRUE(service2.has_value());
  EXPECT_EQ(service2->name, "service2");

  auto nonexistent = manager_->GetService("nonexistent");
  EXPECT_FALSE(nonexistent.has_value());
}

TEST_F(ConfigManagerTest, ServiceMissingName) {
  std::string config = R"({
    "services": [
      {
        "command": "/bin/echo"
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("missing required field: 'name'"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, ServiceMissingCommand) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service"
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("missing required field: 'command'"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, ServiceEmptyName) {
  std::string config = R"({
    "services": [
      {
        "name": "",
        "command": "/bin/echo"
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'name' cannot be empty"), std::string::npos);
}

TEST_F(ConfigManagerTest, ServiceEmptyCommand) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": ""
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'command' cannot be empty"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, ServicesNotAnArray) {
  std::string config = R"({
    "services": "not an array"
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'services' must be an array"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, ServiceNotAnObject) {
  std::string config = R"({
    "services": ["not an object"]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("must be an object"), std::string::npos);
}

TEST_F(ConfigManagerTest, ArgsNotAnArray) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "args": "not an array"
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'args' must be an array"), std::string::npos);
}

TEST_F(ConfigManagerTest, ArgsContainNonString) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "args": ["valid", 123, "string"]
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'args' must contain only strings"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, EnvironmentNotAnObject) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "environment": "not an object"
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'environment' must be an object"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, EnvironmentValueNotString) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "environment": {
          "KEY": 123
        }
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'environment' values must be strings"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, RestartPolicyNever) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_policy": "never"
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].restart_policy,
            aember::service_manager::RestartPolicy::NEVER);
}

TEST_F(ConfigManagerTest, RestartPolicyOnFailure) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_policy": "on-failure"
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].restart_policy,
            aember::service_manager::RestartPolicy::ON_FAILURE);
}

TEST_F(ConfigManagerTest, RestartPolicyOnFailureUnderscore) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_policy": "on_failure"
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].restart_policy,
            aember::service_manager::RestartPolicy::ON_FAILURE);
}

TEST_F(ConfigManagerTest, RestartPolicyAlways) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_policy": "always"
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].restart_policy,
            aember::service_manager::RestartPolicy::ALWAYS);
}

TEST_F(ConfigManagerTest, RestartPolicyInvalid) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_policy": "invalid-policy"
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  // Should default to NEVER
  EXPECT_EQ(services[0].restart_policy,
            aember::service_manager::RestartPolicy::NEVER);
}

TEST_F(ConfigManagerTest, MaxRestartAttemptsValid) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "max_restart_attempts": 20
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].max_restart_attempts, 20);
}

TEST_F(ConfigManagerTest, MaxRestartAttemptsNegative) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "max_restart_attempts": -5
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'max_restart_attempts' must be >= 0"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, RestartDelayValid) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_delay_seconds": 30
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].restart_delay, std::chrono::seconds(30));
}

TEST_F(ConfigManagerTest, RestartDelayNegative) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "restart_delay_seconds": -10
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'restart_delay_seconds' must be >= 0"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, DependenciesValid) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "dependencies": ["dep1", "dep2", "dep3"]
      }
    ]
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));

  auto services = manager_->GetServices();
  ASSERT_EQ(services.size(), 1);
  ASSERT_EQ(services[0].dependencies.size(), 3);
  EXPECT_EQ(services[0].dependencies[0], "dep1");
  EXPECT_EQ(services[0].dependencies[1], "dep2");
  EXPECT_EQ(services[0].dependencies[2], "dep3");
}

TEST_F(ConfigManagerTest, DependenciesNotAnArray) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "dependencies": "not an array"
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'dependencies' must be an array"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, DependenciesContainNonString) {
  std::string config = R"({
    "services": [
      {
        "name": "test-service",
        "command": "/bin/echo",
        "dependencies": ["valid", 123]
      }
    ]
  })";

  EXPECT_FALSE(manager_->LoadFromString(config));

  auto error = manager_->GetLastError();
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find("'dependencies' must contain only strings"),
            std::string::npos);
}

TEST_F(ConfigManagerTest, ClearConfiguration) {
  std::string config = GetBasicValidConfig();

  EXPECT_TRUE(manager_->LoadFromString(config));
  EXPECT_TRUE(manager_->IsLoaded());
  EXPECT_EQ(manager_->GetServices().size(), 1);

  manager_->Clear();

  EXPECT_FALSE(manager_->IsLoaded());
  EXPECT_EQ(manager_->GetServices().size(), 0);
  EXPECT_FALSE(manager_->GetLastError().has_value());
}

TEST_F(ConfigManagerTest, ReloadConfiguration) {
  std::string config1 = GetBasicValidConfig();
  EXPECT_TRUE(manager_->LoadFromString(config1));
  EXPECT_EQ(manager_->GetServices().size(), 1);

  std::string config2 = GetMultipleServicesConfig();
  EXPECT_TRUE(manager_->LoadFromString(config2));
  EXPECT_EQ(manager_->GetServices().size(), 3);
}

TEST_F(ConfigManagerTest, ValidateFileValid) {
  std::string config = GetBasicValidConfig();
  std::string path = CreateTestConfigFile("valid.json", config);

  EXPECT_TRUE(aember::config_manager::ConfigManager::ValidateFile(path));
}

TEST_F(ConfigManagerTest, ValidateFileInvalid) {
  std::string config = R"({ "services": "not an array" })";
  std::string path = CreateTestConfigFile("invalid.json", config);

  aember::config_manager::ConfigError error;
  EXPECT_FALSE(
      aember::config_manager::ConfigManager::ValidateFile(path, &error));
  EXPECT_FALSE(error.message.empty());
}

TEST_F(ConfigManagerTest, ValidateFileNonExistent) {
  aember::config_manager::ConfigError error;
  EXPECT_FALSE(aember::config_manager::ConfigManager::ValidateFile(
      "/non/existent/file.json", &error));
  EXPECT_NE(error.message.find("Failed to open"), std::string::npos);
}

TEST_F(ConfigManagerTest, ValidateJsonValid) {
  nlohmann::json config = nlohmann::json::parse(GetBasicValidConfig());

  EXPECT_TRUE(aember::config_manager::ConfigManager::ValidateJson(config));
}

TEST_F(ConfigManagerTest, ValidateJsonInvalid) {
  nlohmann::json config =
      nlohmann::json::parse(R"({ "services": "not an array" })");

  aember::config_manager::ConfigError error;
  EXPECT_FALSE(
      aember::config_manager::ConfigManager::ValidateJson(config, &error));
  EXPECT_FALSE(error.message.empty());
}

TEST_F(ConfigManagerTest, EmptyServicesArray) {
  std::string config = R"({
    "services": []
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));
  EXPECT_TRUE(manager_->IsLoaded());
  EXPECT_EQ(manager_->GetServices().size(), 0);
}

TEST_F(ConfigManagerTest, NoServicesKey) {
  std::string config = R"({
    "other_key": "value"
  })";

  EXPECT_TRUE(manager_->LoadFromString(config));
  EXPECT_TRUE(manager_->IsLoaded());
  EXPECT_EQ(manager_->GetServices().size(), 0);
}

TEST_F(ConfigManagerTest, LoadFromJsonObject) {
  nlohmann::json config = nlohmann::json::parse(GetBasicValidConfig());

  EXPECT_TRUE(manager_->LoadFromJson(config));
  EXPECT_TRUE(manager_->IsLoaded());

  auto services = manager_->GetServices();
  EXPECT_EQ(services.size(), 1);
  EXPECT_EQ(services[0].name, "test-service");
}

}  // namespace aember_test::config_manager
