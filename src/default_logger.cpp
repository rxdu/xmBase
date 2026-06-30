/*
 * default_logger.cpp
 *
 * Created on 4/20/24 10:34 AM
 * Description:
 *
 * Copyright (c) 2024 Ruixiang Du (rdu)
 */

#include "xmsigma/logging/details/default_logger.hpp"

#include "xmsigma/logging/details/logging_utils.hpp"

namespace xmotion {
DefaultLogger &DefaultLogger::GetInstance() {
  // Construct AND initialize inside the static initializer: the C++ runtime's
  // thread-safe-static guard serializes this, so no other thread can obtain the
  // instance until Initialize() has fully completed (fixes the old init race
  // where a second thread could Log() through a not-yet-assigned logger_).
  // Intentionally leaked — the logger must outlive any static destructor that
  // might log during shutdown.
  static DefaultLogger *instance = [] {
    auto *p = new DefaultLogger();
    p->Initialize(GetCurrentProcessName(), "%^[%l] [%E.%F] [%n]: %v%$", ".log");
    return p;
  }();
  return *instance;
}
}  // namespace xmotion