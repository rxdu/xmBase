/*
 * rt_logger.hpp
 *
 * Hard-real-time logging front-end. Use this only where a loop has an explicit
 * hard deadline; for everything else use XLOG_* (xlogger.hpp), which is the
 * soft-RT async default.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"

#include "xmsigma/logging/details/logger_interface.hpp"

// Compile-time level floor, shared with xlogger.hpp (idempotent definition).
#ifndef XMSIGMA_ACTIVE_LEVEL
#define XMSIGMA_ACTIVE_LEVEL 0
#endif

namespace xmotion {
namespace rt_detail {

// Exception-safe, bounded format into a fixed slot. Shared by the SPSC and MPSC
// hot paths. A malformed format string or bad argument makes fmt throw; on a
// hard-RT thread that unwind is unbounded (and may allocate) and would in
// practice terminate the process — so we catch it and write a fixed diagnostic
// instead. Compiles cleanly with -fno-exceptions (no try/catch then). Returns
// the byte length written (<= cap), never overruns the slot.
template <typename... Args>
inline std::uint16_t FormatInto(char *dst, std::size_t cap,
                                spdlog::string_view_t fmt_str, Args &&...args) {
#if defined(__cpp_exceptions) && __cpp_exceptions
  try {
#endif
    auto res = fmt::format_to_n(dst, cap, fmt::runtime(fmt_str),
                               std::forward<Args>(args)...);
    return static_cast<std::uint16_t>(res.size < cap ? res.size : cap);
#if defined(__cpp_exceptions) && __cpp_exceptions
  } catch (...) {
    static constexpr char kErr[] = "[xmsigma: log format error]";
    std::size_t n = sizeof(kErr) - 1;
    if (n > cap) n = cap;
    std::memcpy(dst, kErr, n);
    return static_cast<std::uint16_t>(n);
  }
#endif
}

}  // namespace rt_detail

// Single-producer / single-consumer lock-free ring logger for hard-RT loops.
//
// Producer (your RT thread) path — wait-free, no heap, no syscall (only a vDSO
// clock read), never blocks:
//   read clock -> format_to_n into the slot's fixed buffer -> release-store the
//   write index. A full ring drops the record and bumps dropped().
// A dedicated drain thread forwards records to spdlog sinks for the actual I/O.
//
// Contract: ONE producer thread per RtLogger. Keep hot-path args trivially
// formattable; messages over kMaxMsgLen are truncated (never reallocated).
//
// Level: the runtime level is seeded from XLOG_LEVEL (so it behaves like the
// XLOG_* soft-RT path) and can be changed with SetLevel(); records below it are
// dropped on the hot path BEFORE any formatting, so a filtered call is nearly
// free. The compile-time XMSIGMA_ACTIVE_LEVEL floor still strips sites entirely.
//
// Timestamp note: records are stamped with system_clock (wall clock) for
// human-readable sink output; it can step on NTP/settimeofday adjustments, so
// do not derive loop intervals from it — use steady_clock for that.
//
// Drain thread: each RtLogger owns one background drain thread that polls and
// performs the sink I/O. Pin it to a housekeeping CPU and run it below your RT
// priority so it never competes with the producer; if you run many loggers,
// prefer sharing one logger across producers (see MpscRtLogger) over spawning
// many drain threads.
class RtLogger {
 public:
  static constexpr std::size_t kMaxMsgLen = 256;  // per-record message cap

  // ring_capacity is rounded up to a power of two.
  explicit RtLogger(std::string name, std::size_t ring_capacity = 4096,
                    bool log_to_file = false);
  ~RtLogger();

  RtLogger(const RtLogger &) = delete;
  RtLogger &operator=(const RtLogger &) = delete;

  // HOT PATH — call only from the single producer (RT) thread.
  template <typename... Args>
  void Log(LogLevel level, spdlog::string_view_t fmt_str, Args &&...args) {
    // Runtime level filter: skip BEFORE the clock read and formatting so a
    // below-threshold call costs only one relaxed load + compare.
    if (level < min_level_.load(std::memory_order_relaxed)) return;
    const uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= capacity_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);  // ring full -> drop
      return;
    }
    Record &rec = ring_[head & mask_];
    rec.level = level;
    rec.time = std::chrono::system_clock::now();  // vDSO clock_gettime, bounded
    rec.len = rt_detail::FormatInto(rec.msg, kMaxMsgLen, fmt_str,
                                    std::forward<Args>(args)...);
    head_.store(head + 1, std::memory_order_release);  // publish
  }

  // Runtime level control. RT-safe to call from any thread (a relaxed store);
  // takes effect on subsequent Log() calls.
  void SetLevel(LogLevel level) {
    min_level_.store(level, std::memory_order_relaxed);
  }
  LogLevel GetLevel() const { return min_level_.load(std::memory_order_relaxed); }

  // Records lost to a full ring (should be 0 in steady state). RT-safe to read.
  std::uint64_t dropped() const {
    return dropped_.load(std::memory_order_relaxed);
  }

  // Block until the ring is drained and the sink flushed. NON-RT — for
  // shutdown/tests only; never call from the hot loop.
  void Flush();

 private:
  struct Record {
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::kInfo;
    std::uint16_t len = 0;
    char msg[kMaxMsgLen];
  };

  void DrainLoop();

  std::size_t capacity_;
  std::size_t mask_;
  std::unique_ptr<Record[]> ring_;
  // Read-mostly runtime level filter (written rarely via SetLevel).
  std::atomic<LogLevel> min_level_{LogLevel::kTrace};
  // Keep producer/consumer indices on separate cache lines (no false sharing).
  alignas(64) std::atomic<uint64_t> head_{0};  // producer writes
  alignas(64) std::atomic<uint64_t> tail_{0};  // consumer writes
  alignas(64) std::atomic<uint64_t> dropped_{0};
  std::atomic<bool> running_{true};
  std::shared_ptr<spdlog::logger> sink_logger_;  // used ONLY by the drain thread
  std::thread drain_thread_;
};

}  // namespace xmotion

// ---- XLOG_RT_* : hard-RT macros (first arg is an RtLogger&) ----------------
#ifdef ENABLE_LOGGING
#define XLOG_RT_IMPL_(rt, lvl, ...) \
  do {                              \
    (rt).Log(lvl, __VA_ARGS__);     \
  } while (0)

#if XMSIGMA_ACTIVE_LEVEL <= 0
#define XLOG_RT_TRACE(rt, ...) \
  XLOG_RT_IMPL_(rt, xmotion::LogLevel::kTrace, __VA_ARGS__)
#else
#define XLOG_RT_TRACE(rt, ...) do {} while (0)
#endif
#if XMSIGMA_ACTIVE_LEVEL <= 1
#define XLOG_RT_DEBUG(rt, ...) \
  XLOG_RT_IMPL_(rt, xmotion::LogLevel::kDebug, __VA_ARGS__)
#else
#define XLOG_RT_DEBUG(rt, ...) do {} while (0)
#endif
#if XMSIGMA_ACTIVE_LEVEL <= 2
#define XLOG_RT_INFO(rt, ...) \
  XLOG_RT_IMPL_(rt, xmotion::LogLevel::kInfo, __VA_ARGS__)
#else
#define XLOG_RT_INFO(rt, ...) do {} while (0)
#endif
#if XMSIGMA_ACTIVE_LEVEL <= 3
#define XLOG_RT_WARN(rt, ...) \
  XLOG_RT_IMPL_(rt, xmotion::LogLevel::kWarn, __VA_ARGS__)
#else
#define XLOG_RT_WARN(rt, ...) do {} while (0)
#endif
#if XMSIGMA_ACTIVE_LEVEL <= 4
#define XLOG_RT_ERROR(rt, ...) \
  XLOG_RT_IMPL_(rt, xmotion::LogLevel::kError, __VA_ARGS__)
#else
#define XLOG_RT_ERROR(rt, ...) do {} while (0)
#endif
#if XMSIGMA_ACTIVE_LEVEL <= 5
#define XLOG_RT_FATAL(rt, ...) \
  XLOG_RT_IMPL_(rt, xmotion::LogLevel::kFatal, __VA_ARGS__)
#else
#define XLOG_RT_FATAL(rt, ...) do {} while (0)
#endif

#else  // ENABLE_LOGGING off
#define XLOG_RT_TRACE(rt, ...) do {} while (0)
#define XLOG_RT_DEBUG(rt, ...) do {} while (0)
#define XLOG_RT_INFO(rt, ...) do {} while (0)
#define XLOG_RT_WARN(rt, ...) do {} while (0)
#define XLOG_RT_ERROR(rt, ...) do {} while (0)
#define XLOG_RT_FATAL(rt, ...) do {} while (0)
#endif
