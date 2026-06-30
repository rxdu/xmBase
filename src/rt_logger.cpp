/*
 * rt_logger.cpp
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmsigma/logging/rt_logger.hpp"

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

RtLogger::RtLogger(std::string name, std::size_t ring_capacity,
                   bool log_to_file) {
  // Round capacity up to a power of two so the index can be masked.
  std::size_t cap = 1;
  while (cap < ring_capacity) cap <<= 1;
  capacity_ = cap;
  mask_ = cap - 1;
  ring_.reset(new Record[cap]);

  // The sink logger is touched ONLY by the drain thread, so a plain synchronous
  // logger is correct here — its I/O is already off the producer's hot path.
  std::vector<spdlog::sink_ptr> sinks;
  if (log_to_file) {
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        CreateLogNameWithFullPath(name, ".rt.log")));
  } else {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }
  sink_logger_ = std::make_shared<spdlog::logger>(name + "_rt", sinks.begin(),
                                                  sinks.end());
  sink_logger_->set_pattern("%^[%l] [%E.%F] [%n]: %v%$");
  sink_logger_->set_level(spdlog::level::trace);

  drain_thread_ = std::thread(&RtLogger::DrainLoop, this);
}

RtLogger::~RtLogger() {
  running_.store(false, std::memory_order_release);
  if (drain_thread_.joinable()) drain_thread_.join();
}

void RtLogger::DrainLoop() {
  for (;;) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {  // ring empty
      if (!running_.load(std::memory_order_acquire)) break;  // stopped + drained
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      continue;
    }
    const Record &rec = ring_[tail & mask_];
    sink_logger_->log(rec.time, spdlog::source_loc{}, ToSpdlogLevel(rec.level),
                      spdlog::string_view_t(rec.msg, rec.len));
    tail_.store(tail + 1, std::memory_order_release);  // free the slot
  }
  sink_logger_->flush();
}

void RtLogger::Flush() {
  // Busy-wait until the consumer catches up. NON-RT (shutdown/tests only).
  while (tail_.load(std::memory_order_acquire) !=
         head_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  sink_logger_->flush();
}

}  // namespace xmotion
