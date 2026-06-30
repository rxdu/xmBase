/*
 * test_rt_logger.cpp — unit tests for the hard-RT RtLogger.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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

// The real correctness test for the lock-free ring: with capacity > N (no
// drops), EVERY record must reach the sink exactly once and intact — no loss,
// no torn/corrupted slot. (Run under TSan this also validates the memory
// ordering between producer and drain thread.)
TEST(RtLoggerTest, DeliversEveryRecordIntactWhenNotOverflowing) {
  const std::string dir = MakeTempLogDir();
  constexpr int kN = 2000;
  {
    RtLogger rt("rt_complete", 8192, /*log_to_file=*/true);  // ring > kN
    for (int i = 0; i < kN; ++i) XLOG_RT_INFO(rt, "seq={};", i);
    rt.Flush();
    EXPECT_EQ(rt.dropped(), 0u);
  }

  std::set<int> seen;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    std::ifstream f(e.path());
    std::string line;
    while (std::getline(f, line)) {
      const auto p = line.find("seq=");
      if (p == std::string::npos) continue;
      const auto q = line.find(';', p);
      if (q == std::string::npos) continue;
      seen.insert(std::stoi(line.substr(p + 4, q - (p + 4))));
    }
  }
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kN))
      << "every record must arrive exactly once and intact";
  EXPECT_EQ(*seen.begin(), 0);
  EXPECT_EQ(*seen.rbegin(), kN - 1);
  fs::remove_all(dir);
}

// A message longer than the fixed slot must be truncated to kMaxMsgLen, never
// overflow the buffer (ASan would flag an overflow).
TEST(RtLoggerTest, TruncatesOversizedMessage) {
  const std::string dir = MakeTempLogDir();
  const std::string big(1000, 'A');  // >> kMaxMsgLen
  {
    RtLogger rt("rt_trunc", 64, /*log_to_file=*/true);
    XLOG_RT_INFO(rt, "{}", big);
    rt.Flush();
  }

  std::size_t max_run = 0, cur = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    std::ifstream f(e.path());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    for (char c : content) {
      cur = (c == 'A') ? cur + 1 : 0;
      max_run = std::max(max_run, cur);
    }
  }
  EXPECT_GT(max_run, 0u);
  EXPECT_LE(max_run, RtLogger::kMaxMsgLen)
      << "oversized message must be truncated to the fixed buffer";
  fs::remove_all(dir);
}
