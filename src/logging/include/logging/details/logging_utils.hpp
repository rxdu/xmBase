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

#include "logging/details/logger_interface.hpp"

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
// Caller supplies the directory; yields "<path>/<prefix>.<YYYYmmddHHMMSS>.csv".
inline std::string CreateLogFileName(std::string prefix, std::string path) {
  time_t t = time(0);
  char buffer[80];
  strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", localtime(&t));
  return path + "/" + prefix + "." + std::string(buffer) + ".csv";
}

// Auto-derives "<logpath>/<YYYYmmdd>/<prefix>-<YYYYmmdd-HHMMSS><suffix>".
inline std::string CreateLogNameWithFullPath(std::string prefix,
                                             std::string suffix) {
  time_t t = time(0);
  struct tm *now = localtime(&t);
  char ts[80];
  strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", now);
  char day[80];
  strftime(day, sizeof(day), "%Y%m%d", now);
  return GetDefaultLogPath() + "/" + std::string(day) + "/" + prefix + "-" +
         std::string(ts) + suffix;
}

}  // namespace xmotion
