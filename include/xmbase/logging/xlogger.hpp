/*
 * xlogger.hpp
 *
 * Created on 4/20/24 10:27 AM
 * Description: Logging front-end — a FACADE over the telemetry event() verb
 *   (ADR 0004 §7): XLOG_* and XM_* are ONE API on one spine, not two logging
 *   systems. Format strings use fmt/{} syntax, NOT printf — e.g.
 *   XLOG_INFO("v = {}", v) — and must be string literals.
 *
 *   Backend today: the interim spdlog binding (telemetry_logging_binding.cpp)
 *   preserves the existing behavior — async spdlog, XLOG_LEVEL runtime level,
 *   env-var config, log files — with zero init calls. When the xmTelemetry
 *   SDK installs its binding, these SAME call sites flow through the SDK's
 *   rings/sinks untouched.
 *
 *   For a hard-real-time loop, use RtLogger / XLOG_RT_* (rt_logger.hpp) until
 *   the SDK's RT channel absorbs that path (P0b).
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#pragma once

#include <sstream>
#include <string>

#include "xmbase/logging/details/default_logger.hpp"  // XLOG_LEVEL control
#include "xmbase/telemetry/event.hpp"                 // the one event funnel

/*
 * level: 0 - TRACE, 1 - DEBUG, 2 - INFO, 3 - WARN,
 *        4 - ERROR, 5 - FATAL, 6 - OFF
 *
 * XMBASE_ACTIVE_LEVEL is a COMPILE-TIME floor: any site below it is compiled
 * out entirely (zero cost), independent of the runtime level. Define it (e.g.
 * -DXMBASE_ACTIVE_LEVEL=2) to strip trace/debug from a release/RT build.
 */
#ifndef XMBASE_ACTIVE_LEVEL
#define XMBASE_ACTIVE_LEVEL 0
#endif

#ifdef ENABLE_LOGGING

// fmt-style log: ONE funnel with XM_* — telemetry EmitEvent. The binding's
// should_log gate short-circuits before argument packing, preserving the old
// "level checked before formatting" behavior.
#define XLOG_IMPL_(sev, ...)                                         \
  do {                                                               \
    ::xmotion::telemetry::detail::EmitEvent(0u, "", sev, __VA_ARGS__); \
  } while (0)

// stream log: gate on the runtime level BEFORE building the (heap-allocating)
// stringstream, so a disabled stream-log in a hot loop costs nothing; the
// built string travels the dynamic-string event path (copied by the backend).
#define XLOG_STREAM_IMPL_(sev, ...)                                          \
  do {                                                                       \
    if (::xmotion::telemetry::ShouldLog(sev)) {                              \
      std::ostringstream _xss;                                               \
      _xss << __VA_ARGS__;                                                   \
      const std::string _xs = _xss.str();                                    \
      ::xmotion::telemetry::detail::EmitEventDyn(0u, "", sev, _xs.data(),    \
                                                 _xs.size());                \
    }                                                                        \
  } while (0)

#define XLOG_LEVEL(level)                                  \
  do {                                                     \
    xmotion::DefaultLogger::GetInstance().SetLoggerLevel(  \
        static_cast<xmotion::LogLevel>(level));            \
  } while (0)
// expression form: usable as `auto l = XLOG_GET_LEVEL();`
#define XLOG_GET_LEVEL() (xmotion::DefaultLogger::GetInstance().GetLoggerLevel())

#if XMBASE_ACTIVE_LEVEL <= 0
#define XLOG_TRACE(...) XLOG_IMPL_(::xmotion::telemetry::Severity::kTrace, __VA_ARGS__)
#define XLOG_TRACE_STREAM(...) \
  XLOG_STREAM_IMPL_(::xmotion::telemetry::Severity::kTrace, __VA_ARGS__)
#else
#define XLOG_TRACE(...) do {} while (0)
#define XLOG_TRACE_STREAM(...) do {} while (0)
#endif

#if XMBASE_ACTIVE_LEVEL <= 1
#define XLOG_DEBUG(...) XLOG_IMPL_(::xmotion::telemetry::Severity::kDebug, __VA_ARGS__)
#define XLOG_DEBUG_STREAM(...) \
  XLOG_STREAM_IMPL_(::xmotion::telemetry::Severity::kDebug, __VA_ARGS__)
#else
#define XLOG_DEBUG(...) do {} while (0)
#define XLOG_DEBUG_STREAM(...) do {} while (0)
#endif

#if XMBASE_ACTIVE_LEVEL <= 2
#define XLOG_INFO(...) XLOG_IMPL_(::xmotion::telemetry::Severity::kInfo, __VA_ARGS__)
#define XLOG_INFO_STREAM(...) \
  XLOG_STREAM_IMPL_(::xmotion::telemetry::Severity::kInfo, __VA_ARGS__)
#else
#define XLOG_INFO(...) do {} while (0)
#define XLOG_INFO_STREAM(...) do {} while (0)
#endif

#if XMBASE_ACTIVE_LEVEL <= 3
#define XLOG_WARN(...) XLOG_IMPL_(::xmotion::telemetry::Severity::kWarn, __VA_ARGS__)
#define XLOG_WARN_STREAM(...) \
  XLOG_STREAM_IMPL_(::xmotion::telemetry::Severity::kWarn, __VA_ARGS__)
#else
#define XLOG_WARN(...) do {} while (0)
#define XLOG_WARN_STREAM(...) do {} while (0)
#endif

#if XMBASE_ACTIVE_LEVEL <= 4
#define XLOG_ERROR(...) XLOG_IMPL_(::xmotion::telemetry::Severity::kError, __VA_ARGS__)
#define XLOG_ERROR_STREAM(...) \
  XLOG_STREAM_IMPL_(::xmotion::telemetry::Severity::kError, __VA_ARGS__)
#else
#define XLOG_ERROR(...) do {} while (0)
#define XLOG_ERROR_STREAM(...) do {} while (0)
#endif

#if XMBASE_ACTIVE_LEVEL <= 5
#define XLOG_FATAL(...) XLOG_IMPL_(::xmotion::telemetry::Severity::kFatal, __VA_ARGS__)
#define XLOG_FATAL_STREAM(...) \
  XLOG_STREAM_IMPL_(::xmotion::telemetry::Severity::kFatal, __VA_ARGS__)
#else
#define XLOG_FATAL(...) do {} while (0)
#define XLOG_FATAL_STREAM(...) do {} while (0)
#endif

#else  // ENABLE_LOGGING not defined — everything compiles to nothing.

#define XLOG_LEVEL(level) do {} while (0)
#define XLOG_GET_LEVEL() (xmotion::LogLevel::kOff)
#define XLOG_TRACE(...) do {} while (0)
#define XLOG_DEBUG(...) do {} while (0)
#define XLOG_INFO(...) do {} while (0)
#define XLOG_WARN(...) do {} while (0)
#define XLOG_ERROR(...) do {} while (0)
#define XLOG_FATAL(...) do {} while (0)
#define XLOG_TRACE_STREAM(...) do {} while (0)
#define XLOG_DEBUG_STREAM(...) do {} while (0)
#define XLOG_INFO_STREAM(...) do {} while (0)
#define XLOG_WARN_STREAM(...) do {} while (0)
#define XLOG_ERROR_STREAM(...) do {} while (0)
#define XLOG_FATAL_STREAM(...) do {} while (0)

#endif
