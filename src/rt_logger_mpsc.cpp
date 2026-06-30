/*
 * rt_logger_mpsc.cpp
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmsigma/logging/rt_logger_mpsc.hpp"

#include <vector>

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "xmsigma/logging/details/logging_utils.hpp"

namespace xmotion {
namespace {
spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace: return spdlog::level::trace;
    case LogLevel::kDebug: return spdlog::level::debug;
    case LogLevel::kInfo: return spdlog::level::info;
    case LogLevel::kWarn: return spdlog::level::warn;
    case LogLevel::kError: return spdlog::level::err;
    case LogLevel::kFatal: return spdlog::level::critical;
    default: return spdlog::level::off;
  }
}
}  // namespace

MpscRtLogger::MpscRtLogger(std::string name, std::size_t ring_capacity,
                           bool log_to_file) {
  std::size_t cap = 1;
  while (cap < ring_capacity) cap <<= 1;
  capacity_ = cap;
  mask_ = cap - 1;
  cells_.reset(new Cell[cap]);
  // Seed each cell's sequence with its index (Vyukov invariant). std::atomic's
  // C++17 default ctor leaves the value indeterminate, so this is required.
  for (std::size_t i = 0; i < cap; ++i)
    cells_[i].sequence.store(i, std::memory_order_relaxed);

  // The sink logger is touched ONLY by the drain thread; a synchronous logger
  // is correct here (its I/O is already off the producers' hot path).
  std::vector<spdlog::sink_ptr> sinks;
  if (log_to_file) {
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        CreateLogNameWithFullPath(name, ".rt.log")));
  } else {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }
  sink_logger_ = std::make_shared<spdlog::logger>(name + "_mrt", sinks.begin(),
                                                  sinks.end());
  sink_logger_->set_pattern("%^[%l] [%E.%F] [%n]: %v%$");
  sink_logger_->set_level(spdlog::level::trace);

  drain_thread_ = std::thread(&MpscRtLogger::DrainLoop, this);
}

MpscRtLogger::~MpscRtLogger() {
  running_.store(false, std::memory_order_release);
  if (drain_thread_.joinable()) drain_thread_.join();
}

void MpscRtLogger::DrainLoop() {
  for (;;) {
    const std::uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    Cell &cell = cells_[pos & mask_];
    const std::uint64_t seq = cell.sequence.load(std::memory_order_acquire);
    const std::int64_t diff =
        static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1);
    if (diff == 0) {  // cell holds the record claimed at `pos`
      sink_logger_->log(cell.time, spdlog::source_loc{},
                        ToSpdlogLevel(cell.level),
                        spdlog::string_view_t(cell.msg, cell.len));
      dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
      cell.sequence.store(pos + mask_ + 1, std::memory_order_release);  // free
    } else {  // empty (no producer has published `pos` yet)
      if (!running_.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  sink_logger_->flush();
}

void MpscRtLogger::Flush() {
  // Wait until the consumer has caught up to every claimed slot. Assumes
  // producers have stopped (enqueue_pos_ is stable). NON-RT.
  while (dequeue_pos_.load(std::memory_order_acquire) !=
         enqueue_pos_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  sink_logger_->flush();
}

}  // namespace xmotion
