/*
 * test_event_logger.cpp — regression tests for the format-string-injection fix.
 *
 * The data loggers must pass payloads as a fmt ARGUMENT ("{}", data), not as the
 * format string; otherwise any '{'/'}' in the payload is parsed as a placeholder
 * and the record is dropped (fmt::format_error -> spdlog error handler).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

#include "xmsigma/logging/event_logger.hpp"

namespace fs = std::filesystem;

namespace {
std::string MakeUniqueTempDir() {
  char tmpl[] = "/tmp/xmsigma_evt_XXXXXX";
  char* path = mkdtemp(tmpl);
  EXPECT_NE(path, nullptr);
  return path == nullptr ? std::string{} : std::string(path);
}
std::string ReadAll(const std::string& dir) {
  std::string all;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    std::ifstream in(e.path());
    all.append((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
  }
  return all;
}
}  // namespace

// Payloads containing '{', '}', and even "{0}" must survive verbatim — on the
// pre-fix code these threw fmt::format_error and the record was dropped.
TEST(EventLoggerTest, BracePayloadIsNotTreatedAsFormatString) {
  const std::string dir = MakeUniqueTempDir();
  ASSERT_FALSE(dir.empty());
  {
    xmotion::EventLogger logger("evt_brace", dir);
    logger.LogInfo("json={\"k\": [0, 1]} and {0} {} placeholders");
    logger.LogEvent(std::string("set{a,b}"), 42);
  }
  spdlog::shutdown();

  const std::string content = ReadAll(dir);
  EXPECT_NE(content.find("json={\"k\": [0, 1]} and {0} {} placeholders"),
            std::string::npos)
      << "brace-laden LogInfo payload must be logged verbatim";
  EXPECT_NE(content.find("set{a,b},42"), std::string::npos)
      << "brace-laden LogEvent payload must be logged verbatim";

  fs::remove_all(dir);
}

// Zero-arg LogEvent must be safely ignored (no pop_back UB), and a later real
// event must still be recorded.
TEST(EventLoggerTest, ZeroArgLogEventIsSafe) {
  const std::string dir = MakeUniqueTempDir();
  ASSERT_FALSE(dir.empty());
  {
    xmotion::EventLogger logger("evt_empty", dir);
    logger.LogEvent();        // no args -> previously UB
    logger.LogEvent(7, 8, 9);
  }
  spdlog::shutdown();

  const std::string content = ReadAll(dir);
  EXPECT_NE(content.find("7,8,9"), std::string::npos);
  fs::remove_all(dir);
}
