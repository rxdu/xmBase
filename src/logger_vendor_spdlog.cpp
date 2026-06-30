/*
 * logger_vendor_spdlog.cpp
 *
 * Created on 4/20/24 10:34 AM
 * Description:
 *
 * Copyright (c) 2024 Ruixiang Du (rdu)
 */

#include "xmsigma/logging/details/logger_vendor_spdlog.hpp"

#include <chrono>

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "xmsigma/logging/details/logging_utils.hpp"

namespace xmotion {
namespace {
// Async queue sizing. One worker thread keeps log ordering deterministic; the
// queue is large enough to absorb bursts before overrun_oldest kicks in.
constexpr std::size_t kAsyncQueueSize = 8192;
constexpr std::size_t kAsyncWorkerThreads = 1;

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return spdlog::level::trace;
    case LogLevel::kDebug:
      return spdlog::level::debug;
    case LogLevel::kInfo:
      return spdlog::level::info;
    case LogLevel::kWarn:
      return spdlog::level::warn;
    case LogLevel::kError:
      return spdlog::level::err;
    case LogLevel::kFatal:
      return spdlog::level::critical;
    default:
      return spdlog::level::off;
  }
}

LogLevel FromSpdlogLevel(spdlog::level::level_enum level) {
  switch (level) {
    case spdlog::level::trace:
      return LogLevel::kTrace;
    case spdlog::level::debug:
      return LogLevel::kDebug;
    case spdlog::level::info:
      return LogLevel::kInfo;
    case spdlog::level::warn:
      return LogLevel::kWarn;
    case spdlog::level::err:
      return LogLevel::kError;
    case spdlog::level::critical:
      return LogLevel::kFatal;
    default:
      return LogLevel::kOff;
  }
}
}  // namespace

void LoggerVendorSpdlog::Initialize(std::string logger_name,
                                    std::string pattern,
                                    std::string file_suffix) {
  // handle log level
  spdlog::level::level_enum log_level = ToSpdlogLevel(default_log_level);
  if (!GetEnvironmentVariable(log_level_env_var_name).empty()) {
    int log_level_int;
    try {
      log_level_int = std::stoi(GetEnvironmentVariable(log_level_env_var_name));
    } catch (std::invalid_argument& e) {
      log_level_int = ToSpdlogLevel(default_log_level);
    }
    if (log_level_int < 0 || log_level_int > 6)
      log_level_int = ToSpdlogLevel(default_log_level);
    log_level = ToSpdlogLevel(static_cast<LogLevel>(log_level_int));
  }

  bool enable_logfile = false;
  std::string enable_logfile_var =
      GetEnvironmentVariable(log_logfile_env_var_name);
  if (enable_logfile_var == "TRUE" || enable_logfile_var == "true" ||
      enable_logfile_var == "1") {
    enable_logfile = true;
  }

  // set up the logger
  sinks_.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  if (enable_logfile && log_level < spdlog::level::off) {
    sinks_.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        CreateLogNameWithFullPath(logger_name, file_suffix)));
  }

  // Async logger: the caller formats + enqueues, a worker thread performs the
  // sink I/O. overrun_oldest => a full queue drops the OLDEST record instead of
  // blocking the producer, so a slow disk or console never stalls a hot loop.
  // (For hard real-time, use RtLogger instead — see rt_logger.hpp.)
  if (!spdlog::thread_pool()) {
    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncWorkerThreads);
  }
  logger_ = std::make_shared<spdlog::async_logger>(
      logger_name, sinks_.begin(), sinks_.end(), spdlog::thread_pool(),
      spdlog::async_overflow_policy::overrun_oldest);
  logger_->set_pattern(pattern);

  // Register so the registry-driven periodic flush below applies to it.
  spdlog::drop(logger_name);  // defensive: clear any prior logger of this name
  spdlog::register_logger(logger_);

  // Do NOT flush on every message. Flush errors promptly; flush routine records
  // periodically. Both flushes are serviced on the worker thread, off the hot
  // path.
  logger_->flush_on(spdlog::level::err);
  spdlog::flush_every(std::chrono::seconds(1));

  SetLoggerLevel(FromSpdlogLevel(log_level));
}

void LoggerVendorSpdlog::Terminate() { logger_->flush(); }

void LoggerVendorSpdlog::SetLoggerLevel(LogLevel level) {
  logger_->set_level(ToSpdlogLevel(level));
}

LogLevel LoggerVendorSpdlog::GetLoggerLevel() {
  return FromSpdlogLevel(logger_->level());
}

bool LoggerVendorSpdlog::ShouldLog(LogLevel level) {
  return logger_->should_log(ToSpdlogLevel(level));
}
}  // namespace xmotion