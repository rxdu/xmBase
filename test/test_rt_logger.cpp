/*
 * test_rt_logger.cpp — properties of the hard-RT RtLogger.
 *
 * The properties that matter for a lock-free SPSC drop-on-full ring:
 *   - Accounting: consumed + dropped == produced (no record silently lost).
 *   - FIFO & uniqueness: consumed seq numbers are STRICTLY increasing (no
 *     reorder, no duplicate, no torn slot) — a std::set would hide both.
 *   - Hot path: no heap allocation, never blocks, drops (not blocks) when full.
 *   - Truncation: oversized messages are clipped to the fixed slot, not OOB.
 *   - Independence: separate RtLoggers don't cross-talk.
 *   - Bounded enqueue latency.
 * Run this file under TSan (lock-free correctness) and ASan/UBSan (no OOB).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "xmsigma/logging/rt_logger.hpp"

// --- per-thread allocation counter, for the no-alloc hot-path test ----------
// thread_local PODs are constant-initialized (no dynamic init -> no allocation
// on first access), so referencing them from operator new can't recurse.
//
// Sanitizers install their own global operator new/delete; overriding them here
// causes alloc/dealloc mismatches, so the override (and the test) are disabled
// under sanitizers — the no-alloc property is checked in the plain build.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define XMSIGMA_SANITIZER_BUILD 1
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define XMSIGMA_SANITIZER_BUILD 1
#endif
#endif

namespace {
thread_local bool t_track_alloc = false;
thread_local std::size_t t_alloc_count = 0;
}  // namespace

#ifndef XMSIGMA_SANITIZER_BUILD
void* operator new(std::size_t n) {
  if (t_track_alloc) ++t_alloc_count;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace fs = std::filesystem;
using xmotion::RtLogger;

namespace {

std::string MakeTempLogDir() {
  char tmpl[] = "/tmp/xmsigma_rt_XXXXXX";
  char* dir = mkdtemp(tmpl);
  EXPECT_NE(dir, nullptr);
  setenv("XLOG_FOLDER", dir, 1);
  return std::string(dir);
}

// Extract every "seq=<n>;" token from the dir's files, IN FILE ORDER (= the
// drain thread's consume order).
std::vector<uint64_t> ReadSeqsInOrder(const std::string& dir) {
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file()) files.push_back(e.path());
  std::sort(files.begin(), files.end());

  std::vector<uint64_t> seqs;
  for (const auto& f : files) {
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line)) {
      const auto p = line.find("seq=");
      if (p == std::string::npos) continue;
      const auto q = line.find(';', p);
      if (q == std::string::npos) continue;
      seqs.push_back(std::stoull(line.substr(p + 4, q - (p + 4))));
    }
  }
  return seqs;
}

bool FileTreeContains(const std::string& dir, const std::string& needle) {
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    std::ifstream f(e.path());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Assert a consumed-seq sequence is strictly increasing (FIFO + no dup + no
// torn/garbage slot) and lies within [0, produced).
void ExpectStrictlyIncreasingInRange(const std::vector<uint64_t>& seqs,
                                     uint64_t produced) {
  for (std::size_t i = 1; i < seqs.size(); ++i) {
    ASSERT_LT(seqs[i - 1], seqs[i])
        << "records must be FIFO with no duplicates (i=" << i << ")";
  }
  if (!seqs.empty()) {
    EXPECT_LT(seqs.back(), produced) << "no seq beyond what was produced";
  }
}

}  // namespace

// A known message survives the ring + drain thread and reaches the file.
TEST(RtLoggerTest, WritesKnownMessageToFile) {
  const std::string dir = MakeTempLogDir();
  const std::string marker = "rt_logger_marker_98765";
  {
    RtLogger rt("rt_file", 1024, /*log_to_file=*/true);
    XLOG_RT_INFO(rt, "{}", marker);
    rt.Flush();
  }
  EXPECT_TRUE(FileTreeContains(dir, marker));
  fs::remove_all(dir);
}

// No drops when the ring comfortably outpaces the message rate, and EVERY
// record arrives exactly once, intact, in order.
TEST(RtLoggerTest, DeliversEveryRecordInOrderWhenNotOverflowing) {
  const std::string dir = MakeTempLogDir();
  constexpr uint64_t kN = 5000;
  RtLogger rt("rt_complete", 16384, /*log_to_file=*/true);
  for (uint64_t i = 0; i < kN; ++i) XLOG_RT_INFO(rt, "seq={};", i);
  rt.Flush();
  EXPECT_EQ(rt.dropped(), 0u);

  const std::vector<uint64_t> seqs = ReadSeqsInOrder(dir);
  ASSERT_EQ(seqs.size(), kN) << "every record must be delivered exactly once";
  ExpectStrictlyIncreasingInRange(seqs, kN);
  EXPECT_EQ(seqs.front(), 0u);
  EXPECT_EQ(seqs.back(), kN - 1);
  fs::remove_all(dir);
}

// The core lock-free correctness test: a dedicated producer thread floods a
// small ring (forcing drops) while the drain consumes. Verify exact accounting
// (consumed + dropped == produced) and FIFO/uniqueness of the consumed records.
TEST(RtLoggerTest, ConcurrentProducerFifoAndExactAccounting) {
  const std::string dir = MakeTempLogDir();
  constexpr uint64_t kN = 100000;
  uint64_t dropped = 0;
  {
    RtLogger rt("rt_acct", 256, /*log_to_file=*/true);  // small -> drops likely
    std::thread producer([&] {
      for (uint64_t i = 0; i < kN; ++i) XLOG_RT_INFO(rt, "seq={};", i);
    });
    producer.join();
    rt.Flush();
    dropped = rt.dropped();
  }  // dtor joins the drain thread

  const std::vector<uint64_t> seqs = ReadSeqsInOrder(dir);
  ExpectStrictlyIncreasingInRange(seqs, kN);
  EXPECT_EQ(seqs.size() + dropped, kN)
      << "every produced record is either consumed or counted as dropped";
  EXPECT_LE(dropped, kN);
  fs::remove_all(dir);
}

// Drop-on-full: a tiny ring flooded far faster than the I/O-bound drain MUST
// drop, and the accounting must still balance.
TEST(RtLoggerTest, DropsWhenRingFullWithBalancedAccounting) {
  const std::string dir = MakeTempLogDir();
  constexpr uint64_t kN = 200000;
  uint64_t dropped = 0;
  {
    RtLogger rt("rt_drop", 2, /*log_to_file=*/true);  // 2-slot ring
    for (uint64_t i = 0; i < kN; ++i) XLOG_RT_INFO(rt, "seq={};", i);
    rt.Flush();
    dropped = rt.dropped();
  }
  const std::vector<uint64_t> seqs = ReadSeqsInOrder(dir);
  EXPECT_GT(dropped, 0u) << "a 2-slot ring under a 200k burst must overflow";
  ExpectStrictlyIncreasingInRange(seqs, kN);
  EXPECT_EQ(seqs.size() + dropped, kN);
  fs::remove_all(dir);
}

// The hot path must not heap-allocate (the defining hard-RT property). Count
// allocations on the producer thread across a batch of Log() calls.
TEST(RtLoggerTest, HotPathDoesNotAllocate) {
#ifdef XMSIGMA_SANITIZER_BUILD
  GTEST_SKIP() << "allocation counting is disabled under sanitizers";
#else
  const std::string dir = MakeTempLogDir();
  RtLogger rt("rt_noalloc", 8192, /*log_to_file=*/true);
  XLOG_RT_INFO(rt, "warmup {}", 0);  // absorb any one-time init

  t_alloc_count = 0;
  t_track_alloc = true;
  for (int i = 0; i < 2000; ++i) {
    XLOG_RT_INFO(rt, "seq={}; v={} f={}", i, i * 2, i * 0.5);
  }
  t_track_alloc = false;

  EXPECT_EQ(t_alloc_count, 0u)
      << "RtLogger::Log must not allocate on the hot path";
  rt.Flush();
  fs::remove_all(dir);
#endif  // XMSIGMA_SANITIZER_BUILD
}

// A message longer than the fixed slot is truncated to kMaxMsgLen, never
// overflows it (ASan would flag an overflow).
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
  EXPECT_LE(max_run, RtLogger::kMaxMsgLen) << "must clip to the fixed buffer";
  fs::remove_all(dir);
}

// A non-power-of-two capacity rounds up and still delivers everything; a
// capacity of 1 is valid.
TEST(RtLoggerTest, NonPowerOfTwoCapacityRoundsUp) {
  const std::string dir = MakeTempLogDir();
  RtLogger rt("rt_cap", 1000, /*log_to_file=*/true);  // rounds to 1024
  for (uint64_t i = 0; i < 500; ++i) XLOG_RT_INFO(rt, "seq={};", i);
  rt.Flush();
  EXPECT_EQ(rt.dropped(), 0u) << "1000->1024 capacity must hold 500 records";
  EXPECT_EQ(ReadSeqsInOrder(dir).size(), 500u);
  fs::remove_all(dir);
}

TEST(RtLoggerTest, CapacityOfOneWorks) {
  const std::string dir = MakeTempLogDir();
  RtLogger rt("rt_cap1", 1, /*log_to_file=*/true);
  for (uint64_t i = 0; i < 1000; ++i) XLOG_RT_INFO(rt, "seq={};", i);
  rt.Flush();
  // Some delivered, accounting balances, FIFO preserved.
  const std::vector<uint64_t> seqs = ReadSeqsInOrder(dir);
  ExpectStrictlyIncreasingInRange(seqs, 1000);
  EXPECT_EQ(seqs.size() + rt.dropped(), 1000u);
  fs::remove_all(dir);
}

// Independent RtLoggers, each with its own producer thread and file, must not
// cross-talk (no shared mutable state between rings/drain threads).
TEST(RtLoggerTest, IndependentLoggersDoNotCrossTalk) {
  const std::string dir = MakeTempLogDir();
  constexpr int kLoggers = 3;
  constexpr uint64_t kN = 3000;

  std::vector<std::unique_ptr<RtLogger>> loggers;
  for (int k = 0; k < kLoggers; ++k)
    loggers.push_back(std::make_unique<RtLogger>(
        "rt_multi_" + std::to_string(k), 16384, /*log_to_file=*/true));

  std::vector<std::thread> producers;
  for (int k = 0; k < kLoggers; ++k) {
    producers.emplace_back([&, k] {
      for (uint64_t i = 0; i < kN; ++i)
        XLOG_RT_INFO(*loggers[k], "L{} seq={};", k, i);
    });
  }
  for (auto& t : producers) t.join();
  for (auto& lg : loggers) lg->Flush();
  for (auto& lg : loggers) EXPECT_EQ(lg->dropped(), 0u);

  // Each per-logger file (named rt_multi_<k>-...) must contain ONLY that
  // logger's own "L<k> " tag and exactly kN records; the global total must be
  // kLoggers*kN (nothing lost, nothing crossed between rings).
  std::size_t total = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const std::string fname = e.path().filename().string();
    int owner = -1;
    for (int k = 0; k < kLoggers; ++k)
      if (fname.find("rt_multi_" + std::to_string(k)) != std::string::npos)
        owner = k;
    ASSERT_GE(owner, 0) << "unexpected file " << fname;
    const std::string own_tag = "L" + std::to_string(owner) + " ";

    std::ifstream f(e.path());
    std::string line;
    std::size_t in_file = 0;
    while (std::getline(f, line)) {
      if (line.find("seq=") == std::string::npos) continue;
      ++in_file;
      ++total;
      EXPECT_NE(line.find(own_tag), std::string::npos)
          << fname << " holds a record from another logger: " << line;
    }
    EXPECT_EQ(in_file, kN) << fname << " must have all kN records";
  }
  EXPECT_EQ(total, static_cast<std::size_t>(kLoggers) * kN);
  loggers.clear();
  fs::remove_all(dir);
}

// Enqueue latency must be bounded (loose ceiling — catches gross regressions /
// hidden blocking, not micro-jitter). Reports the distribution.
TEST(RtLoggerTest, EnqueueLatencyIsBounded) {
  const std::string dir = MakeTempLogDir();
  RtLogger rt("rt_lat", 8192, /*log_to_file=*/true);
  constexpr int kN = 20000;
  std::vector<uint64_t> ns;
  ns.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    XLOG_RT_INFO(rt, "seq={}; v={}", i, i);
    const auto t1 = std::chrono::steady_clock::now();
    ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }
  rt.Flush();
  std::sort(ns.begin(), ns.end());
  const uint64_t p50 = ns[kN / 2];
  const uint64_t p99 = ns[kN * 99 / 100];
  std::cerr << "[RtLogger enqueue] p50=" << p50 << "ns p99=" << p99
            << "ns max=" << ns.back() << "ns\n";
  // Generous bound: a correct wait-free enqueue is sub-microsecond; 1ms p99
  // would mean it blocked. Avoids flakiness on shared CI runners.
  EXPECT_LT(p99, 1000000u) << "p99 enqueue latency must be well under 1ms";
  fs::remove_all(dir);
}
