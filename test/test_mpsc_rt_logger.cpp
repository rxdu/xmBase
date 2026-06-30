/*
 * test_mpsc_rt_logger.cpp — properties of the multi-producer RtLogger.
 *
 * Beyond the single-producer properties, the MPSC ring must hold under
 * CONCURRENT producers:
 *   - Global accounting: total_consumed + dropped == total_produced.
 *   - Per-producer FIFO + uniqueness: each producer's seq numbers appear in the
 *     output in STRICTLY increasing order (the consumer reads in claim order,
 *     and a producer claims its own records in order) — catches reorder,
 *     duplicate, and torn cells under contention.
 *   - Hot path: no heap allocation.
 * Run under TSan (the real multi-producer race check) and ASan/UBSan.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "xmsigma/logging/rt_logger_mpsc.hpp"

// --- per-thread allocation counter (disabled under sanitizers; see test_rt) --
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
using xmotion::LogLevel;
using xmotion::MpscRtLogger;

namespace {

std::string MakeTempLogDir() {
  char tmpl[] = "/tmp/xmsigma_mrt_XXXXXX";
  char* dir = mkdtemp(tmpl);
  EXPECT_NE(dir, nullptr);
  setenv("XLOG_FOLDER", dir, 1);
  return std::string(dir);
}

// Parse "p=<producer>;s=<seq>;" tokens from all files, IN FILE ORDER (= the
// drain thread's consume order).
struct Record {
  int producer;
  uint64_t seq;
};
std::vector<Record> ReadRecordsInOrder(const std::string& dir) {
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file()) files.push_back(e.path());
  std::sort(files.begin(), files.end());

  std::vector<Record> recs;
  for (const auto& f : files) {
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line)) {
      const auto pp = line.find("p=");
      const auto sp = line.find(";s=");
      if (pp == std::string::npos || sp == std::string::npos) continue;
      const auto se = line.find(';', sp + 3);
      if (se == std::string::npos) continue;
      Record r;
      r.producer = std::stoi(line.substr(pp + 2, sp - (pp + 2)));
      r.seq = std::stoull(line.substr(sp + 3, se - (sp + 3)));
      recs.push_back(r);
    }
  }
  return recs;
}

// Strictly-increasing seq PER producer (FIFO + no dup + no torn slot).
void ExpectPerProducerFifo(const std::vector<Record>& recs, int n_producers) {
  std::map<int, int64_t> last;  // producer -> last seq seen (-1 = none)
  for (int k = 0; k < n_producers; ++k) last[k] = -1;
  for (const auto& r : recs) {
    ASSERT_GE(r.producer, 0);
    ASSERT_LT(r.producer, n_producers);
    ASSERT_GT(static_cast<int64_t>(r.seq), last[r.producer])
        << "producer " << r.producer
        << " records must be FIFO with no duplicates";
    last[r.producer] = static_cast<int64_t>(r.seq);
  }
}

}  // namespace

// Cornerstone: many producers flood a small ring (forcing drops). Every record
// is consumed or counted-dropped, and each producer's records stay FIFO.
TEST(MpscRtLoggerTest, MultiProducerAccountingAndPerProducerFifo) {
  const std::string dir = MakeTempLogDir();
  constexpr int kProducers = 4;
  constexpr uint64_t kPer = 50000;
  uint64_t dropped = 0;
  {
    MpscRtLogger rt("mrt_acct", 1024, /*log_to_file=*/true);  // small -> drops
    std::vector<std::thread> threads;
    for (int k = 0; k < kProducers; ++k) {
      threads.emplace_back([&, k] {
        for (uint64_t i = 0; i < kPer; ++i) XLOG_RT_INFO(rt, "p={};s={};", k, i);
      });
    }
    for (auto& t : threads) t.join();
    rt.Flush();
    dropped = rt.dropped();
  }  // dtor joins drain

  const std::vector<Record> recs = ReadRecordsInOrder(dir);
  ExpectPerProducerFifo(recs, kProducers);
  EXPECT_EQ(recs.size() + dropped, kProducers * kPer)
      << "every produced record is consumed or counted as dropped";
  fs::remove_all(dir);
}

// With an ample ring, no drops: every producer's full 0..kPer-1 is delivered.
TEST(MpscRtLoggerTest, NoDropsWhenRingAmple) {
  const std::string dir = MakeTempLogDir();
  constexpr int kProducers = 4;
  constexpr uint64_t kPer = 1000;
  {
    MpscRtLogger rt("mrt_full", 1 << 14, /*log_to_file=*/true);  // 16384 >> work
    std::vector<std::thread> threads;
    for (int k = 0; k < kProducers; ++k) {
      threads.emplace_back([&, k] {
        for (uint64_t i = 0; i < kPer; ++i) XLOG_RT_INFO(rt, "p={};s={};", k, i);
      });
    }
    for (auto& t : threads) t.join();
    rt.Flush();
    EXPECT_EQ(rt.dropped(), 0u);
  }
  const std::vector<Record> recs = ReadRecordsInOrder(dir);
  ASSERT_EQ(recs.size(), kProducers * kPer);
  ExpectPerProducerFifo(recs, kProducers);
  // each producer delivered exactly kPer records (0..kPer-1)
  std::map<int, uint64_t> counts;
  for (const auto& r : recs) ++counts[r.producer];
  for (int k = 0; k < kProducers; ++k) EXPECT_EQ(counts[k], kPer);
  fs::remove_all(dir);
}

// A tiny ring under concurrent flood must drop, and accounting must balance.
TEST(MpscRtLoggerTest, DropsWhenRingFull) {
  const std::string dir = MakeTempLogDir();
  constexpr int kProducers = 4;
  constexpr uint64_t kPer = 20000;
  uint64_t dropped = 0;
  {
    MpscRtLogger rt("mrt_drop", 2, /*log_to_file=*/true);
    std::vector<std::thread> threads;
    for (int k = 0; k < kProducers; ++k)
      threads.emplace_back([&, k] {
        for (uint64_t i = 0; i < kPer; ++i) XLOG_RT_INFO(rt, "p={};s={};", k, i);
      });
    for (auto& t : threads) t.join();
    rt.Flush();
    dropped = rt.dropped();
  }
  const std::vector<Record> recs = ReadRecordsInOrder(dir);
  EXPECT_GT(dropped, 0u);
  ExpectPerProducerFifo(recs, kProducers);
  EXPECT_EQ(recs.size() + dropped, kProducers * kPer);
  fs::remove_all(dir);
}

// The hot path must not heap-allocate (single thread is enough to measure it).
TEST(MpscRtLoggerTest, HotPathDoesNotAllocate) {
#ifdef XMSIGMA_SANITIZER_BUILD
  GTEST_SKIP() << "allocation counting is disabled under sanitizers";
#else
  const std::string dir = MakeTempLogDir();
  MpscRtLogger rt("mrt_noalloc", 8192, /*log_to_file=*/true);
  XLOG_RT_INFO(rt, "warmup {}", 0);

  t_alloc_count = 0;
  t_track_alloc = true;
  for (int i = 0; i < 2000; ++i) XLOG_RT_INFO(rt, "p=0;s={}; v={}", i, i * 2);
  t_track_alloc = false;

  EXPECT_EQ(t_alloc_count, 0u)
      << "MpscRtLogger::Log must not allocate on the hot path";
  rt.Flush();
  fs::remove_all(dir);
#endif
}
