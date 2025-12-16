#include <aember-libs-tests/utils/logging/logging.h>

#include <spdlog/spdlog.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace aember_test::utils::logging {

void LoggingTest::SetUp() {
  // Initialize logging once per test binary
  static bool initialized = false;
  if (!initialized) {
    aember::utils::init_early_logging();
    spdlog::set_level(spdlog::level::trace);
    initialized = true;
  }

  log_file_path_ = "/tmp/aember_logging_test.log";

  // Clean previous runs
  fs::remove(log_file_path_);
}

void LoggingTest::TearDown() {
  // Flush to ensure file writes are complete
  spdlog::shutdown();
}

aember::utils::Logger LoggingTest::make_logger(const std::string& name) {
  return aember::utils::Logger(name);
}

/* ----------------------------- Tests ----------------------------- */

TEST_F(LoggingTest, LoggerCreationDoesNotThrow) {
  EXPECT_NO_THROW({
    auto log = make_logger("test.logger.creation");
    log.info("hello");
  });
}

TEST_F(LoggingTest, AllLogLevelsWork) {
  auto log = make_logger("test.logger.levels");

  EXPECT_NO_THROW(log.trace("trace"));
  EXPECT_NO_THROW(log.debug("debug"));
  EXPECT_NO_THROW(log.info("info"));
  EXPECT_NO_THROW(log.warn("warn"));
  EXPECT_NO_THROW(log.error("error"));
  EXPECT_NO_THROW(log.critical("critical"));
}

TEST_F(LoggingTest, FileLoggingCanBeEnabled) {
  aember::utils::enable_file_logging(log_file_path_);

  auto log = make_logger("test.logger.file");
  log.info("file logging works");

  spdlog::shutdown();

  ASSERT_TRUE(fs::exists(log_file_path_));
  ASSERT_GT(fs::file_size(log_file_path_), 0);
}

TEST_F(LoggingTest, EnablingFileLoggingTwiceIsSafe) {
  EXPECT_NO_THROW(aember::utils::enable_file_logging(log_file_path_));
  EXPECT_NO_THROW(aember::utils::enable_file_logging(log_file_path_));

  auto log = make_logger("test.logger.double");
  log.info("double enable safe");

  spdlog::shutdown();

  ASSERT_TRUE(fs::exists(log_file_path_));
}

TEST_F(LoggingTest, MultipleLoggersShareSinks) {
  aember::utils::enable_file_logging(log_file_path_);

  auto log1 = make_logger("logger.one");
  auto log2 = make_logger("logger.two");

  log1.info("from logger one");
  log2.info("from logger two");

  spdlog::shutdown();

  ASSERT_TRUE(fs::exists(log_file_path_));
  ASSERT_GT(fs::file_size(log_file_path_), 0);
}

}  // namespace aember_test::utils::logging
