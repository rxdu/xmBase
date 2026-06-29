/*
 * @file default_logger.hpp
 * @date 4/20/24
 * @brief
 *
 * @copyright Copyright (c) 2024 Ruixiang Du (rdu)
 */

#pragma once

#include <memory>
#include <atomic>

#include "logging/details/logger_vendor_spdlog.hpp"

namespace xmotion {
class DefaultLogger final : public LoggerVendorSpdlog {
  DefaultLogger() : LoggerVendorSpdlog() {}

 public:
  static std::shared_ptr<DefaultLogger> GetInstance();
  ~DefaultLogger() = default;

  // do not allow copy
  DefaultLogger(const DefaultLogger &other) = delete;
  DefaultLogger &operator=(const DefaultLogger &other) = delete;
};
}  // namespace xmotion

