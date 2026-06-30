/*
 * test_xlogger.cpp
 *
 * Description: Unit tests for the XLOG_* macros and the DefaultLogger level
 *              API. Each TEST is run by ctest in its own process (via
 *              gtest_discover_tests), so the one-time logger initialization
 *              and any environment overrides do not leak between cases.
 *
 * Copyright (c) 2024 Ruixiang Du (rdu)
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

#include "xmsigma/logging/xlogger.hpp"
#include "xmsigma/logging/details/default_logger.hpp"

namespace fs = std::filesystem;

using xmotion::DefaultLogger;
using xmotion::LogLevel;

namespace {

int CurrentLevel() {
  return static_cast<int>(DefaultLogger::GetInstance().GetLoggerLevel());
}

// Streamable probe that records whether its operator<< actually ran — used to
// prove the _STREAM macros do NOT build the message when the level is disabled.
int g_probe_evals = 0;
struct EvalProbe {};
std::ostream& operator<<(std::ostream& os, const EvalProbe&) {
  ++g_probe_evals;
  return os << "probed";
}

}  // namespace

TEST(XLoggerTest, SetLevelIsReflectedByGetLevel) {
  XLOG_LEVEL(static_cast<int>(LogLevel::kInfo));
  EXPECT_EQ(CurrentLevel(), static_cast<int>(LogLevel::kInfo));

  XLOG_LEVEL(static_cast<int>(LogLevel::kError));
  EXPECT_EQ(CurrentLevel(), static_cast<int>(LogLevel::kError));

  XLOG_LEVEL(static_cast<int>(LogLevel::kTrace));
  EXPECT_EQ(CurrentLevel(), static_cast<int>(LogLevel::kTrace));

  XLOG_LEVEL(static_cast<int>(LogLevel::kOff));
  EXPECT_EQ(CurrentLevel(), static_cast<int>(LogLevel::kOff));
}

TEST(XLoggerTest, LoggingMacrosDoNotThrow) {
  XLOG_LEVEL(static_cast<int>(LogLevel::kTrace));
  EXPECT_NO_THROW({
    XLOG_TRACE("trace message");
    XLOG_DEBUG("debug message");
    XLOG_INFO("info message");
    XLOG_WARN("warn message");
    XLOG_ERROR("error message");
    XLOG_FATAL("fatal message");
    XLOG_INFO_STREAM("stream value: " << 42);
  });
}

TEST(XLoggerTest, WritesKnownMessageToLogFile) {
  // Redirect the log folder to a unique temp dir and enable file logging.
  // These env vars are consumed during the one-time DefaultLogger init, so
  // they must be set before the first XLOG_* call in this process.
  char tmpl[] = "/tmp/xmsigma_xlog_XXXXXX";
  char* dir = mkdtemp(tmpl);
  ASSERT_NE(dir, nullptr);
  ASSERT_EQ(setenv("XLOG_FOLDER", dir, 1), 0);
  ASSERT_EQ(setenv("XLOG_ENABLE_LOGFILE", "1", 1), 0);

  const std::string marker = "xlogger_unit_test_marker_42";
  XLOG_LEVEL(static_cast<int>(LogLevel::kInfo));
  XLOG_INFO("{}", marker);

  // Flush/drain any buffered records before reading the file back.
  DefaultLogger::GetInstance().Terminate();
  spdlog::shutdown();

  // The file sink writes to <XLOG_FOLDER>/<date>/<proc>-<timestamp>.log.
  bool found_marker = false;
  bool found_any_log = false;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".log")
      continue;
    found_any_log = true;
    std::ifstream in(entry.path());
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_FALSE(content.empty()) << "log file should not be empty";
    if (content.find(marker) != std::string::npos) found_marker = true;
  }

  EXPECT_TRUE(found_any_log) << "expected a .log file under " << dir;
  EXPECT_TRUE(found_marker) << "expected the logged marker in the log file";

  fs::remove_all(dir);
}

// The _STREAM macros must gate on the level BEFORE building the message, so a
// disabled stream-log in a hot loop evaluates none of its arguments.
TEST(XLoggerTest, StreamMacroSkipsArgEvalWhenLevelDisabled) {
  g_probe_evals = 0;

  XLOG_LEVEL(static_cast<int>(LogLevel::kError));  // debug disabled
  XLOG_DEBUG_STREAM("v=" << EvalProbe{});
  EXPECT_EQ(g_probe_evals, 0) << "disabled stream-log must not evaluate args";

  XLOG_LEVEL(static_cast<int>(LogLevel::kTrace));  // debug enabled
  XLOG_DEBUG_STREAM("v=" << EvalProbe{});
  EXPECT_EQ(g_probe_evals, 1) << "enabled stream-log must evaluate args once";
}

// Macros must be usable as a single statement in an unbraced if/else — this
// compiles only because they are do-while(0) wrapped (the old {} blocks orphan
// the else). The successful compile IS the assertion.
TEST(XLoggerTest, MacrosComposeInUnbracedIfElse) {
  int x = 1;
  if (x == 2)
    XLOG_INFO("two");
  else
    XLOG_INFO("not two");

  for (int i = 0; i < 2; ++i)
    if (i == 0)
      XLOG_TRACE("t");
    else
      XLOG_DEBUG("d");

  SUCCEED();
}
