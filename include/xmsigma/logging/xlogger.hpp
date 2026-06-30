/*
 * xlogger.hpp
 *
 * Created on 4/20/24 10:27 AM
 * Description: Soft-real-time logging front-end (async spdlog behind it).
 *   Format strings use fmt/{} syntax, NOT printf — e.g. XLOG_INFO("v = {}", v).
 *   For a hard-real-time loop, use RtLogger / XLOG_RT_* (rt_logger.hpp).
 *
 * Copyright (c) 2024 Ruixiang Du (rdu)
 */

#pragma once

#include <sstream>

#include "xmsigma/logging/details/default_logger.hpp"

/*
 * level: 0 - TRACE, 1 - DEBUG, 2 - INFO, 3 - WARN,
 *        4 - ERROR, 5 - FATAL, 6 - OFF
 *
 * XMSIGMA_ACTIVE_LEVEL is a COMPILE-TIME floor: any site below it is compiled
 * out entirely (zero cost), independent of the runtime level. Define it (e.g.
 * -DXMSIGMA_ACTIVE_LEVEL=2) to strip trace/debug from a release/RT build.
 */
#ifndef XMSIGMA_ACTIVE_LEVEL
#define XMSIGMA_ACTIVE_LEVEL 0
#endif

#ifdef ENABLE_LOGGING

// fmt-style log: spdlog short-circuits on the level before formatting.
#define XLOG_IMPL_(lvl, ...)                                   \
  do {                                                         \
    xmotion::DefaultLogger::GetInstance().Log(lvl, __VA_ARGS__); \
  } while (0)

// stream log: gate on the level BEFORE building the (heap-allocating)
// stringstream, so a disabled stream-log in a hot loop costs nothing.
#define XLOG_STREAM_IMPL_(lvl, ...)                  \
  do {                                               \
    auto &_xlg = xmotion::DefaultLogger::GetInstance(); \
    if (_xlg.ShouldLog(lvl)) {                       \
      std::ostringstream _xss;                       \
      _xss << __VA_ARGS__;                           \
      _xlg.Log(lvl, "{}", _xss.str());               \
    }                                                \
  } while (0)

#define XLOG_LEVEL(level)                                  \
  do {                                                     \
    xmotion::DefaultLogger::GetInstance().SetLoggerLevel(  \
        static_cast<xmotion::LogLevel>(level));            \
  } while (0)
// expression form: usable as `auto l = XLOG_GET_LEVEL();`
#define XLOG_GET_LEVEL() (xmotion::DefaultLogger::GetInstance().GetLoggerLevel())

#if XMSIGMA_ACTIVE_LEVEL <= 0
#define XLOG_TRACE(...) XLOG_IMPL_(xmotion::LogLevel::kTrace, __VA_ARGS__)
#define XLOG_TRACE_STREAM(...) \
  XLOG_STREAM_IMPL_(xmotion::LogLevel::kTrace, __VA_ARGS__)
#else
#define XLOG_TRACE(...) do {} while (0)
#define XLOG_TRACE_STREAM(...) do {} while (0)
#endif

#if XMSIGMA_ACTIVE_LEVEL <= 1
#define XLOG_DEBUG(...) XLOG_IMPL_(xmotion::LogLevel::kDebug, __VA_ARGS__)
#define XLOG_DEBUG_STREAM(...) \
  XLOG_STREAM_IMPL_(xmotion::LogLevel::kDebug, __VA_ARGS__)
#else
#define XLOG_DEBUG(...) do {} while (0)
#define XLOG_DEBUG_STREAM(...) do {} while (0)
#endif

#if XMSIGMA_ACTIVE_LEVEL <= 2
#define XLOG_INFO(...) XLOG_IMPL_(xmotion::LogLevel::kInfo, __VA_ARGS__)
#define XLOG_INFO_STREAM(...) \
  XLOG_STREAM_IMPL_(xmotion::LogLevel::kInfo, __VA_ARGS__)
#else
#define XLOG_INFO(...) do {} while (0)
#define XLOG_INFO_STREAM(...) do {} while (0)
#endif

#if XMSIGMA_ACTIVE_LEVEL <= 3
#define XLOG_WARN(...) XLOG_IMPL_(xmotion::LogLevel::kWarn, __VA_ARGS__)
#define XLOG_WARN_STREAM(...) \
  XLOG_STREAM_IMPL_(xmotion::LogLevel::kWarn, __VA_ARGS__)
#else
#define XLOG_WARN(...) do {} while (0)
#define XLOG_WARN_STREAM(...) do {} while (0)
#endif

#if XMSIGMA_ACTIVE_LEVEL <= 4
#define XLOG_ERROR(...) XLOG_IMPL_(xmotion::LogLevel::kError, __VA_ARGS__)
#define XLOG_ERROR_STREAM(...) \
  XLOG_STREAM_IMPL_(xmotion::LogLevel::kError, __VA_ARGS__)
#else
#define XLOG_ERROR(...) do {} while (0)
#define XLOG_ERROR_STREAM(...) do {} while (0)
#endif

#if XMSIGMA_ACTIVE_LEVEL <= 5
#define XLOG_FATAL(...) XLOG_IMPL_(xmotion::LogLevel::kFatal, __VA_ARGS__)
#define XLOG_FATAL_STREAM(...) \
  XLOG_STREAM_IMPL_(xmotion::LogLevel::kFatal, __VA_ARGS__)
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
