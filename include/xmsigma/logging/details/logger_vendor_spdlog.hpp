/*
 * @file logger_vendor_spdlog.hpp
 * @date 4/20/24
 * @brief
 *
 * @copyright Copyright (c) 2024 Ruixiang Du (rdu)
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <iostream>

#include "spdlog/spdlog.h"

#include "xmsigma/logging/details/logger_interface.hpp"

namespace xmotion {
class LoggerVendorSpdlog : public LoggerInterface {
 public:
  LoggerVendorSpdlog() = default;
  virtual ~LoggerVendorSpdlog() = default;

  // public methods
  void Initialize(std::string logger_name, std::string pattern,
                  std::string file_suffix) override;
  void Terminate() override;

  void SetLoggerLevel(LogLevel level) override;
  LogLevel GetLoggerLevel() override;

  // Cheap runtime level test — lets call sites skip building a message that
  // would be filtered out (used by the *_STREAM macros before formatting).
  bool ShouldLog(LogLevel level);

  template <typename... LogArgs>
  void Log(LogLevel level, LogArgs&&... args) {
    switch (level) {
      case LogLevel::kTrace:
        logger_->trace(std::forward<LogArgs>(args)...);
        break;
      case LogLevel::kDebug:
        logger_->debug(std::forward<LogArgs>(args)...);
        break;
      case LogLevel::kInfo:
        logger_->info(std::forward<LogArgs>(args)...);
        break;
      case LogLevel::kWarn:
        logger_->warn(std::forward<LogArgs>(args)...);
        break;
      case LogLevel::kError:
        logger_->error(std::forward<LogArgs>(args)...);
        break;
      case LogLevel::kFatal:
        logger_->critical(std::forward<LogArgs>(args)...);
        break;
      default:
        break;
    }
  }

 protected:
  std::string process_name_ = "uninitialized";
  std::shared_ptr<spdlog::logger> logger_;
  std::vector<spdlog::sink_ptr> sinks_;
};
}  // namespace xmotion

