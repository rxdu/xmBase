/*
 * test_csv_logger.cpp
 *
 * Description: Unit tests for xmotion::CsvLogger. Verifies that LogData()
 *              produces a CSV file on disk whose rows match the logged values.
 *
 * Copyright (c) 2024 Ruixiang Du (rdu)
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

#include "xmsigma/logging/csv_logger.hpp"

namespace fs = std::filesystem;

namespace {

// Create a guaranteed-unique, empty temporary directory.
std::string MakeUniqueTempDir() {
  char tmpl[] = "/tmp/xmsigma_csv_XXXXXX";
  char* path = mkdtemp(tmpl);
  EXPECT_NE(path, nullptr) << "mkdtemp failed to create a temp directory";
  return path == nullptr ? std::string{} : std::string(path);
}

// Find the single generated *.csv file in the directory.
std::vector<fs::path> FindCsvFiles(const std::string& dir) {
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".csv") {
      files.push_back(entry.path());
    }
  }
  return files;
}

// Read non-empty lines from a file.
std::vector<std::string> ReadLines(const fs::path& file) {
  std::vector<std::string> lines;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

}  // namespace

TEST(CsvLoggerTest, WritesLoggedRowsToCsvFile) {
  const std::string dir = MakeUniqueTempDir();
  ASSERT_FALSE(dir.empty());

  {
    xmotion::CsvLogger logger("test_csv", dir);
    // Use integers so std::to_string formatting is deterministic.
    logger.LogData(1, 2, 3);
    logger.LogData(4, 5, 6);
    logger.LogData(7, 8, 9);
  }

  // CsvLogger uses spdlog's async factory, so log records may still be sitting
  // in the background queue. spdlog::shutdown() destroys the thread pool, which
  // drains every pending record to its sink before joining, guaranteeing the
  // file is fully written before we read it back.
  spdlog::shutdown();

  const std::vector<fs::path> csv_files = FindCsvFiles(dir);
  ASSERT_EQ(csv_files.size(), 1u)
      << "expected exactly one .csv file in " << dir;

  const std::vector<std::string> lines = ReadLines(csv_files.front());
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "1,2,3");
  EXPECT_EQ(lines[1], "4,5,6");
  EXPECT_EQ(lines[2], "7,8,9");

  fs::remove_all(dir);
}

// Regression: LogData() with no fields used to pop_back() an empty string (UB).
// It must be safely ignored, not corrupt the stream or crash.
TEST(CsvLoggerTest, ZeroArgLogDataIsSafelyIgnored) {
  const std::string dir = MakeUniqueTempDir();
  ASSERT_FALSE(dir.empty());

  {
    xmotion::CsvLogger logger("test_csv_empty", dir);
    logger.LogData();         // no fields -> previously UB
    logger.LogData(10, 20);   // a real row in between
    logger.LogData();         // again
  }
  spdlog::shutdown();

  const auto csv_files = FindCsvFiles(dir);
  ASSERT_EQ(csv_files.size(), 1u);
  const auto lines = ReadLines(csv_files.front());
  ASSERT_EQ(lines.size(), 1u) << "empty LogData() calls must produce no rows";
  EXPECT_EQ(lines[0], "10,20");

  fs::remove_all(dir);
}
