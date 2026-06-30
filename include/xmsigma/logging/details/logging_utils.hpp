/*
 * logging_utils.hpp
 *
 * Path, environment and process helpers for the logging module.
 * Consolidated from the former details/ (2018) and src/ (2024) utilities into
 * a single, install-safe header.
 *
 * Copyright (c) 2018-2026 Ruixiang Du (rdu)
 */

#pragma once

#include <ctime>
#include <cstdlib>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "xmsigma/logging/details/logger_interface.hpp"

namespace xmotion {

// --- environment ---------------------------------------------------------
inline std::string GetEnvironmentVariable(const std::string &name) {
#if defined(_WIN32) || defined(_WIN64)
  char buffer[32767];  // max env var size on Windows
  DWORD n = GetEnvironmentVariableA(name.c_str(), buffer, sizeof(buffer));
  return (n > 0 && n <= sizeof(buffer)) ? std::string(buffer) : std::string();
#else
  const char *value = std::getenv(name.c_str());
  return value ? std::string(value) : std::string();
#endif
}

// Parse XLOG_LEVEL (0..6) into a LogLevel, falling back when unset or invalid.
// Uses strtol (not stoi) so it never throws — safe to call from -fno-exceptions
// builds and from constructors on any path.
inline LogLevel ResolveLogLevelFromEnv(LogLevel fallback = default_log_level) {
  const std::string value = GetEnvironmentVariable(log_level_env_var_name);
  if (value.empty()) return fallback;
  char *end = nullptr;
  const long level = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || level < 0 || level > 6) return fallback;
  return static_cast<LogLevel>(level);
}

// --- process -------------------------------------------------------------
inline std::string GetCurrentProcessName() {
#if defined(_WIN32) || defined(_WIN64)
  char buffer[MAX_PATH];
  GetModuleFileNameA(NULL, buffer, MAX_PATH);
  std::string full(buffer);
  return full.substr(full.find_last_of("\\/") + 1);
#elif defined(__linux__)
  char buffer[1024];
  ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (len == -1) return "";
  buffer[len] = '\0';
  std::string full(buffer);
  return full.substr(full.find_last_of('/') + 1);
#elif defined(__APPLE__)
  char buffer[1024];
  return proc_name(getpid(), buffer, sizeof(buffer)) > 0 ? std::string(buffer)
                                                         : std::string();
#else
  return "Unsupported Platform";
#endif
}

// --- paths ---------------------------------------------------------------
// Root for xMotion runtime data. Falls back to a temp dir when $HOME is unset
// (CI/daemon contexts) instead of dereferencing a null pointer or hardcoding a
// personal path.
inline std::string GetDefaultRootPath() {
  const char *home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.xmotion";
}

// Default log folder: "<root>/log".
inline std::string GetLogFolderPath() { return GetDefaultRootPath() + "/log"; }

// Like GetLogFolderPath, but honors the XLOG_FOLDER override when set — used by
// the runtime file logger.
inline std::string GetDefaultLogPath() {
  std::string configured = GetEnvironmentVariable(log_folder_env_var_name);
  return configured.empty() ? GetLogFolderPath() : configured;
}

// --- filename builders ---------------------------------------------------
// Thread-safe localtime: localtime() returns a pointer into a shared static
// struct, so two threads building filenames concurrently can corrupt each
// other's result. Use the reentrant variant.
inline struct tm LocalTimeNow(time_t t) {
  struct tm out {};
#if defined(_WIN32) || defined(_WIN64)
  localtime_s(&out, &t);
#else
  localtime_r(&t, &out);
#endif
  return out;
}

// Caller supplies the directory; yields "<path>/<prefix>.<YYYYmmddHHMMSS>.csv".
inline std::string CreateLogFileName(std::string prefix, std::string path) {
  struct tm now = LocalTimeNow(time(0));
  char buffer[80];
  strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &now);
  return path + "/" + prefix + "." + std::string(buffer) + ".csv";
}

// Auto-derives "<logpath>/<YYYYmmdd>/<prefix>-<YYYYmmdd-HHMMSS><suffix>".
inline std::string CreateLogNameWithFullPath(std::string prefix,
                                             std::string suffix) {
  struct tm now = LocalTimeNow(time(0));
  char ts[80];
  strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &now);
  char day[80];
  strftime(day, sizeof(day), "%Y%m%d", &now);
  return GetDefaultLogPath() + "/" + std::string(day) + "/" + prefix + "-" +
         std::string(ts) + suffix;
}

}  // namespace xmotion
