/*
 * test_ctrl_logger.cpp — unit tests for xmotion::CtrlLogger.
 *
 * Covers the header/data round-trip and the out-of-range-id guard (the id-based
 * AddItemDataToEntry used to write past the vector — UB). Each TEST runs in its
 * own process (gtest_discover_tests), so the GetLogger() singleton is fresh.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "xmsigma/logging/ctrl_logger.hpp"

namespace fs = std::filesystem;
using xmotion::CtrlLogger;

namespace {
std::string MakeTempDir() {
  char tmpl[] = "/tmp/xmsigma_ctrl_XXXXXX";
  char* p = mkdtemp(tmpl);
  EXPECT_NE(p, nullptr);
  return p == nullptr ? std::string{} : std::string(p);
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
// After Flush(), the async sink drains on a worker thread; poll (bounded) until
// `needle` appears rather than sleeping a fixed amount.
std::string WaitForContent(const std::string& dir, const std::string& needle) {
  std::string content;
  for (int i = 0; i < 400; ++i) {
    content = ReadAll(dir);
    if (content.find(needle) != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return content;
}
}  // namespace

TEST(CtrlLoggerTest, RoundTripsHeaderAndData) {
  const std::string dir = MakeTempDir();
  ASSERT_FALSE(dir.empty());

  CtrlLogger& log = CtrlLogger::GetLogger("ctrl_rt", dir);
  log.AddItemNameToEntryHead("a");
  log.AddItemNameToEntryHead("b");
  log.PassEntryHeaderToLogger();  // marks head added, sizes the data row
  log.AddItemDataToEntry(std::string("a"), 1.0);
  log.AddItemDataToEntry(std::string("b"), 2.0);
  log.PassEntryDataToLogger();
  log.Flush();

  const std::string content = WaitForContent(dir, "1.000000 , 2.000000");
  EXPECT_NE(content.find("a , b"), std::string::npos) << "header row";
  EXPECT_NE(content.find("1.000000 , 2.000000"), std::string::npos)
      << "data row";

  fs::remove_all(dir);
}

// The id-based overload must reject an out-of-range id instead of writing past
// the vector (previously UB). No crash; ASan would flag an OOB write.
TEST(CtrlLoggerTest, OutOfRangeIdIsGuarded) {
  const std::string dir = MakeTempDir();
  ASSERT_FALSE(dir.empty());

  CtrlLogger& log = CtrlLogger::GetLogger("ctrl_bounds", dir);
  log.AddItemNameToEntryHead("x");
  log.PassEntryHeaderToLogger();  // item_data_ now has size 1 (valid id: 0)

  log.AddItemDataToEntry(static_cast<std::uint64_t>(999), std::string("oob"));
  log.AddItemDataToEntry(static_cast<std::uint64_t>(0), std::string("ok"));
  log.PassEntryDataToLogger();
  log.Flush();

  SUCCEED();  // reaching here without crash/OOB is the assertion
  fs::remove_all(dir);
}
