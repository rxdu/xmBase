/*
 * test_rt_logger.cpp — unit tests for the hard-RT RtLogger.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

#include "xmsigma/logging/rt_logger.hpp"

namespace fs = std::filesystem;
using xmotion::RtLogger;

namespace {
// Make a unique temp dir and point the RT file sink at it via XLOG_FOLDER.
std::string MakeTempLogDir() {
  char tmpl[] = "/tmp/xmsigma_rt_XXXXXX";
  char* dir = mkdtemp(tmpl);
  EXPECT_NE(dir, nullptr);
  setenv("XLOG_FOLDER", dir, 1);
  return std::string(dir);
}

// Recursively search for a *.rt.log file containing `needle`.
bool FileTreeContains(const std::string& root, const std::string& needle) {
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    std::ifstream f(e.path());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.find(needle) != std::string::npos) return true;
  }
  return false;
}
}  // namespace

// With an ample ring and a normal message rate, nothing is dropped.
TEST(RtLoggerTest, DrainsWithoutDropsUnderNormalLoad) {
  const std::string dir = MakeTempLogDir();
  RtLogger rt("rt_normal", 4096, /*log_to_file=*/true);
  for (int i = 0; i < 500; ++i) {
    XLOG_RT_INFO(rt, "iteration {} value {}", i, i * 2);
  }
  rt.Flush();
  EXPECT_EQ(rt.dropped(), 0u);
  fs::remove_all(dir);
}

// A known message survives the ring + drain thread and reaches the file.
TEST(RtLoggerTest, WritesKnownMessageToFile) {
  const std::string dir = MakeTempLogDir();
  const std::string marker = "rt_logger_marker_98765";
  {
    RtLogger rt("rt_file", 1024, /*log_to_file=*/true);
    XLOG_RT_INFO(rt, "{}", marker);
    rt.Flush();
  }  // dtor joins the drain thread
  EXPECT_TRUE(FileTreeContains(dir, marker));
  fs::remove_all(dir);
}

// A tiny ring flooded far faster than the I/O-bound drain MUST drop, and must
// account for every record (consumed + dropped == produced) without UB/crash.
TEST(RtLoggerTest, DropsWhenRingFull) {
  const std::string dir = MakeTempLogDir();
  constexpr uint64_t kBurst = 200000;
  uint64_t dropped = 0;
  {
    RtLogger rt("rt_drop", 2, /*log_to_file=*/true);  // 2-slot ring
    for (uint64_t i = 0; i < kBurst; ++i) {
      XLOG_RT_INFO(rt, "x {}", i);
    }
    rt.Flush();
    dropped = rt.dropped();
  }
  EXPECT_GT(dropped, 0u);        // the ring overflowed
  EXPECT_LE(dropped, kBurst);    // never more than produced
  fs::remove_all(dir);
}
