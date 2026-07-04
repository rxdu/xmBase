/*
 * test_logging.cpp  (formerly test_xlogger.cpp)
 *
 * Description: Unit tests for the unified XM_* logging macros — the event()
 *              verb of the telemetry API — running through the interim spdlog
 *              binding (auto-adopted on first use), plus the runtime level
 *              API. Each TEST is run by ctest in its own process (via
 *              gtest_discover_tests), so the one-time logger initialization
 *              and any environment overrides do not leak between cases.
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

#include "xmbase/logging/details/default_logger.hpp"
#include "xmbase/telemetry/telemetry.hpp"

namespace fs = std::filesystem;
namespace tel = xmotion::telemetry;

using xmotion::DefaultLogger;
using tel::Severity;

namespace {

// Streamable probe that records whether its operator<< actually ran — used to
// prove the _STREAM macros do NOT build the message when the level is disabled.
int g_probe_evals = 0;
struct EvalProbe {};
std::ostream& operator<<(std::ostream& os, const EvalProbe&) {
  ++g_probe_evals;
  return os << "probed";
}

}  // namespace

TEST(LoggingTest, SetLevelIsReflectedByGetLevel) {
  // Routed through the interim binding to DefaultLogger's runtime level.
  tel::SetLogLevel(Severity::kInfo);
  EXPECT_EQ(tel::GetLogLevel(), Severity::kInfo);

  tel::SetLogLevel(Severity::kError);
  EXPECT_EQ(tel::GetLogLevel(), Severity::kError);

  tel::SetLogLevel(Severity::kTrace);
  EXPECT_EQ(tel::GetLogLevel(), Severity::kTrace);

  tel::SetLogLevel(Severity::kOff);  // filter value: silence everything
  EXPECT_EQ(tel::GetLogLevel(), Severity::kOff);
}

TEST(LoggingTest, LoggingMacrosDoNotThrow) {
  tel::SetLogLevel(Severity::kTrace);
  EXPECT_NO_THROW({
    XM_TRACE("trace message");
    XM_DEBUG("debug message");
    XM_INFO("info message");
    XM_WARN("warn message");
    XM_ERROR("error message");
    XM_FATAL("fatal message");
    XM_INFO_STREAM("stream value: " << 42);
  });
}

TEST(LoggingTest, WritesKnownMessageToLogFile) {
  // Redirect the log folder to a unique temp dir and enable file logging.
  // These env vars are consumed during the one-time DefaultLogger init, so
  // they must be set before the first emit in this process. (Env spelling
  // stays XLOG_* until the SDK owns configuration — see design doc P0b.)
  char tmpl[] = "/tmp/xmbase_xlog_XXXXXX";
  char* dir = mkdtemp(tmpl);
  ASSERT_NE(dir, nullptr);
  ASSERT_EQ(setenv("XLOG_FOLDER", dir, 1), 0);
  ASSERT_EQ(setenv("XLOG_ENABLE_LOGFILE", "1", 1), 0);

  const std::string marker = "logging_unit_test_marker_42";
  tel::SetLogLevel(Severity::kInfo);
  XM_INFO("{} pi={:.3f}", marker, 3.14159);  // spec fidelity via arg store

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
    if (content.find(marker + " pi=3.142") != std::string::npos)
      found_marker = true;
  }

  EXPECT_TRUE(found_any_log) << "expected a .log file under " << dir;
  EXPECT_TRUE(found_marker)
      << "expected the marker WITH fmt-spec-formatted value in the log file";

  fs::remove_all(dir);
}

// The _STREAM macros must gate on the level BEFORE building the message, so a
// disabled stream-log in a hot loop evaluates none of its arguments.
TEST(LoggingTest, StreamMacroSkipsArgEvalWhenLevelDisabled) {
  g_probe_evals = 0;

  tel::SetLogLevel(Severity::kError);  // debug disabled
  XM_DEBUG_STREAM("v=" << EvalProbe{});
  EXPECT_EQ(g_probe_evals, 0) << "disabled stream-log must not evaluate args";

  tel::SetLogLevel(Severity::kTrace);  // debug enabled
  XM_DEBUG_STREAM("v=" << EvalProbe{});
  EXPECT_EQ(g_probe_evals, 1) << "enabled stream-log must evaluate args once";
}

// Macros must be usable as a single statement in an unbraced if/else — this
// compiles only because they are do-while(0)/expression wrapped. The
// successful compile IS the assertion.
TEST(LoggingTest, MacrosComposeInUnbracedIfElse) {
  int x = 1;
  if (x == 2)
    XM_INFO("two");
  else
    XM_INFO("not two");

  for (int i = 0; i < 2; ++i)
    if (i == 0)
      XM_TRACE("t");
    else
      XM_DEBUG("d");

  SUCCEED();
}
