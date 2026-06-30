/*
 * @file default_logger.hpp
 * @date 4/20/24
 * @brief
 *
 * @copyright Copyright (c) 2024 Ruixiang Du (rdu)
 */

#pragma once

#include "xmsigma/logging/details/logger_vendor_spdlog.hpp"

namespace xmotion {
class DefaultLogger final : public LoggerVendorSpdlog {
  DefaultLogger() : LoggerVendorSpdlog() {}

 public:
  // Process-wide logger. The first call constructs AND initializes it inside the
  // static-init guard, so concurrent callers can never observe a
  // half-initialized logger. Returns a reference (no shared_ptr refcount traffic
  // on the hot path).
  static DefaultLogger &GetInstance();

  // do not allow copy
  DefaultLogger(const DefaultLogger &other) = delete;
  DefaultLogger &operator=(const DefaultLogger &other) = delete;
};
}  // namespace xmotion

