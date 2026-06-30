/*
 * rt_logger_mpsc.hpp
 *
 * Multi-producer / single-consumer hard-RT logger. Same role and contract as
 * RtLogger, but ANY number of producer threads may Log() concurrently (e.g.
 * several control loops or sensor callbacks sharing one logger). Use RtLogger
 * (SPSC, wait-free) when there is exactly one producer; use this when there is
 * more than one.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"

// Pulls in LogLevel and the logger-agnostic XLOG_RT_* macros (they only call
// .Log(), so they work for an MpscRtLogger too).
#include "xmsigma/logging/rt_logger.hpp"

namespace xmotion {

// Bounded Vyukov MPSC ring: each cell carries a sequence number that encodes
// its state. A producer claims a slot with a CAS on the shared enqueue index,
// formats into the slot's fixed buffer, then publishes by advancing the cell's
// sequence. The single consumer (drain thread) reads slots strictly in claim
// order, so per-producer FIFO is preserved even though producers may publish
// out of order.
//
// Hot path: lock-free (a producer's claim retries at most once per *competing*
// producer — bounded by the producer count, vs RtLogger's wait-free single
// producer), no heap, no syscall (vDSO clock only), never blocks. A full ring
// drops the record and bumps dropped().
class MpscRtLogger {
 public:
  static constexpr std::size_t kMaxMsgLen = 256;

  explicit MpscRtLogger(std::string name, std::size_t ring_capacity = 4096,
                        bool log_to_file = false);
  ~MpscRtLogger();

  MpscRtLogger(const MpscRtLogger &) = delete;
  MpscRtLogger &operator=(const MpscRtLogger &) = delete;

  // HOT PATH — safe to call from any number of threads concurrently.
  template <typename... Args>
  void Log(LogLevel level, spdlog::string_view_t fmt_str, Args &&...args) {
    Cell *cell;
    std::uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::uint64_t seq = cell->sequence.load(std::memory_order_acquire);
      const std::int64_t diff =
          static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
      if (diff == 0) {
        // Cell is free for this pos — try to claim it.
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                               std::memory_order_relaxed))
          break;
      } else if (diff < 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);  // ring full
        return;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);  // lost race; retry
      }
    }
    cell->time = std::chrono::system_clock::now();  // vDSO clock, bounded
    cell->level = level;
    auto res = fmt::format_to_n(cell->msg, kMaxMsgLen, fmt::runtime(fmt_str),
                                std::forward<Args>(args)...);
    cell->len = static_cast<std::uint16_t>(res.size < kMaxMsgLen ? res.size
                                                                 : kMaxMsgLen);
    cell->sequence.store(pos + 1, std::memory_order_release);  // publish
  }

  std::uint64_t dropped() const {
    return dropped_.load(std::memory_order_relaxed);
  }

  // Block until the ring is drained and the sink flushed. NON-RT — call only
  // after producers have stopped (shutdown/tests).
  void Flush();

 private:
  struct Cell {
    std::atomic<std::uint64_t> sequence;
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::kInfo;
    std::uint16_t len = 0;
    char msg[kMaxMsgLen];
  };

  void DrainLoop();

  std::size_t capacity_;
  std::size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(64) std::atomic<std::uint64_t> enqueue_pos_{0};  // contended by producers
  alignas(64) std::atomic<std::uint64_t> dequeue_pos_{0};  // single consumer
  alignas(64) std::atomic<std::uint64_t> dropped_{0};
  std::atomic<bool> running_{true};
  std::shared_ptr<spdlog::logger> sink_logger_;  // touched only by the drain
  std::thread drain_thread_;
};

}  // namespace xmotion
