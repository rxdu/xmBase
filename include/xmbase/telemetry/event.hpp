/*
 * telemetry/event.hpp
 *
 * The event() verb: discrete structured records with deferred formatting
 * (NanoLog/Quill pattern). The hot path copies ARGUMENTS into a bounded
 * stack pack; formatting happens at the drain (bound) or in the stderr
 * fallback (unbound, Warn+ only — ADR 0004 §2).
 *
 * Format strings use "{}" placeholders and MUST be string literals (the SDK
 * interns by pointer). Supported argument types: arithmetic, string_view,
 * const char* — anything richer is formatted by the caller OFF the hot path.
 *
 * Attribution (delta D8): events carry an explicit EventSource — a
 * pre-registered, interned subsystem tag ("imu", "motor_left", ...). The
 * sourceless macro forms attribute to the default source. In a driver layer,
 * explicitness about WHICH device is speaking is worth the extra argument.
 *
 * Compile-out floor (S6-A3): XM_TELEMETRY_LEVEL strips below-floor MACRO call
 * sites entirely (0=trace … 5=fatal, 6=off). Function-call APIs (handles)
 * are not macro-strippable; they compile to no-op slot writes instead.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <sstream>
#include <string>
#include <string_view>

#include "xmbase/telemetry/binding.hpp"
#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/time.hpp"

namespace xmotion {
namespace telemetry {

// Interned subsystem tag (value type, like the metric handles).
class EventSource {
 public:
  std::uint32_t id() const noexcept { return id_; }
  const char* name() const noexcept { return name_; }

 private:
  friend EventSource GetEventSource(const char*) noexcept;
  EventSource(std::uint32_t id, const char* name) noexcept
      : id_(id), name_(name) {}
  std::uint32_t id_;
  const char* name_;  // string literal, kept for the unbound fallback
};

// Register (or look up) a subsystem tag. `name` must be a string literal.
// id 0 is the default/anonymous source.
inline EventSource GetEventSource(const char* name) noexcept {
  const Binding* b = ActiveBinding();
  return EventSource(b != nullptr ? b->intern_source(name) : 0u, name);
}

// Runtime level gate: true if an event at `sev` would currently be recorded.
// Bound: the binding's runtime level (the SDK's, or DefaultLogger's for the
// interim logging binding). Unbound: the stderr threshold (Warn+). Use this
// to skip expensive message construction (the XLOG_*_STREAM macros do).
inline bool ShouldLog(Severity sev) noexcept {
  const Binding* b = ActiveBinding();
  if (b != nullptr) return b->should_log == nullptr || b->should_log(sev);
  return sev >= Severity::kWarn;
}

namespace detail {

// Single funnel behind the macros. `fmt` and (for the unbound path)
// `source_name` must be string literals.
template <typename... Args>
void EmitEvent(std::uint32_t source_id, const char* source_name, Severity sev,
               const char* fmt, Args&&... args) noexcept {
  const Binding* b = ActiveBinding();
  if (b != nullptr) {
    if (b->should_log != nullptr && !b->should_log(sev)) return;  // pre-pack
    const ArgPack pack = PackArgs(std::forward<Args>(args)...);
    b->emit_event(source_id, sev, fmt, pack, CurrentContext(), Now());
  } else if (sev >= Severity::kWarn) {
    const ArgPack pack = PackArgs(std::forward<Args>(args)...);
    UnboundEmitEvent(source_name, sev, fmt, pack);
  }
}

// Dynamic-string funnel (pre-formatted messages; the XLOG stream path).
inline void EmitEventDyn(std::uint32_t source_id, const char* source_name,
                         Severity sev, const char* msg,
                         std::size_t len) noexcept {
  const Binding* b = ActiveBinding();
  if (b != nullptr) {
    if (b->should_log != nullptr && !b->should_log(sev)) return;
    b->emit_event_dyn(source_id, sev, msg, len, CurrentContext(), Now());
  } else if (sev >= Severity::kWarn) {
    UnboundEmitEventDyn(source_name, sev, msg, len);
  }
}

}  // namespace detail

// Process/robot identity (OTel "resource"); set once at startup, non-RT.
inline void SetResource(std::string_view key, std::string_view value) {
  const Binding* b = ActiveBinding();
  if (b != nullptr) b->set_resource(key, value);
}

// Runtime minimum-severity control (formerly XLOG_LEVEL / XLOG_GET_LEVEL).
// Routed through the binding (interim: DefaultLogger's level; SDK: its own).
// Unbound: set is a no-op and get reports the fixed stderr threshold (kWarn).
inline void SetLogLevel(Severity min_sev) noexcept {
  const Binding* b = ActiveBinding();
  if (b != nullptr && b->set_level != nullptr) b->set_level(min_sev);
}
inline Severity GetLogLevel() noexcept {
  const Binding* b = ActiveBinding();
  if (b != nullptr && b->get_level != nullptr) return b->get_level();
  return Severity::kWarn;
}

}  // namespace telemetry
}  // namespace xmotion

// ---- macros -------------------------------------------------------------------

// Severity floor: call sites strictly below XM_TELEMETRY_LEVEL compile to
// nothing (not even argument evaluation). 0 keeps everything; 6 strips all.
#ifndef XM_TELEMETRY_LEVEL
#define XM_TELEMETRY_LEVEL 0
#endif

#define XM_TELEMETRY_DETAIL_EVENT(src, sev, ...)                             \
  ::xmotion::telemetry::detail::EmitEvent((src).id(), (src).name(),          \
                                          ::xmotion::telemetry::sev,         \
                                          __VA_ARGS__)
#define XM_TELEMETRY_DETAIL_EVENT_NOSRC(sev, ...)                            \
  ::xmotion::telemetry::detail::EmitEvent(0u, "", ::xmotion::telemetry::sev, \
                                          __VA_ARGS__)
#define XM_TELEMETRY_DETAIL_NOOP(...) \
  do {                                \
  } while (false)
// Stream form (formerly XLOG_*_STREAM): gate on the RUNTIME level BEFORE
// building the heap-allocating stringstream, so a disabled stream-log in a
// hot loop costs one ShouldLog() check; the built string travels the
// dynamic-string event path (copied by the backend). Non-RT convenience.
#define XM_TELEMETRY_DETAIL_STREAM(sev, ...)                                  \
  do {                                                                        \
    if (::xmotion::telemetry::ShouldLog(::xmotion::telemetry::sev)) {         \
      std::ostringstream _xm_ss;                                              \
      _xm_ss << __VA_ARGS__;                                                  \
      const std::string _xm_s = _xm_ss.str();                                 \
      ::xmotion::telemetry::detail::EmitEventDyn(0u, "",                      \
                                                 ::xmotion::telemetry::sev,   \
                                                 _xm_s.data(), _xm_s.size()); \
    }                                                                         \
  } while (false)

// Sourceless forms (attribute to the default source).
#if XM_TELEMETRY_LEVEL <= 0
#define XM_TRACE(...) XM_TELEMETRY_DETAIL_EVENT_NOSRC(Severity::kTrace, __VA_ARGS__)
#define XM_TRACE_STREAM(...) XM_TELEMETRY_DETAIL_STREAM(Severity::kTrace, __VA_ARGS__)
#else
#define XM_TRACE(...) XM_TELEMETRY_DETAIL_NOOP()
#define XM_TRACE_STREAM(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 1
#define XM_DEBUG(...) XM_TELEMETRY_DETAIL_EVENT_NOSRC(Severity::kDebug, __VA_ARGS__)
#define XM_DEBUG_STREAM(...) XM_TELEMETRY_DETAIL_STREAM(Severity::kDebug, __VA_ARGS__)
#else
#define XM_DEBUG(...) XM_TELEMETRY_DETAIL_NOOP()
#define XM_DEBUG_STREAM(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 2
#define XM_INFO(...) XM_TELEMETRY_DETAIL_EVENT_NOSRC(Severity::kInfo, __VA_ARGS__)
#define XM_INFO_STREAM(...) XM_TELEMETRY_DETAIL_STREAM(Severity::kInfo, __VA_ARGS__)
#else
#define XM_INFO(...) XM_TELEMETRY_DETAIL_NOOP()
#define XM_INFO_STREAM(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 3
#define XM_WARN(...) XM_TELEMETRY_DETAIL_EVENT_NOSRC(Severity::kWarn, __VA_ARGS__)
#define XM_WARN_STREAM(...) XM_TELEMETRY_DETAIL_STREAM(Severity::kWarn, __VA_ARGS__)
#else
#define XM_WARN(...) XM_TELEMETRY_DETAIL_NOOP()
#define XM_WARN_STREAM(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 4
#define XM_ERROR(...) XM_TELEMETRY_DETAIL_EVENT_NOSRC(Severity::kError, __VA_ARGS__)
#define XM_ERROR_STREAM(...) XM_TELEMETRY_DETAIL_STREAM(Severity::kError, __VA_ARGS__)
#else
#define XM_ERROR(...) XM_TELEMETRY_DETAIL_NOOP()
#define XM_ERROR_STREAM(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 5
#define XM_FATAL(...) XM_TELEMETRY_DETAIL_EVENT_NOSRC(Severity::kFatal, __VA_ARGS__)
#define XM_FATAL_STREAM(...) XM_TELEMETRY_DETAIL_STREAM(Severity::kFatal, __VA_ARGS__)
#else
#define XM_FATAL(...) XM_TELEMETRY_DETAIL_NOOP()
#define XM_FATAL_STREAM(...) XM_TELEMETRY_DETAIL_NOOP()
#endif

// Source-attributed forms (delta D8): XM_WARN_SRC(imu_src, "fmt {}", v).
#if XM_TELEMETRY_LEVEL <= 0
#define XM_TRACE_SRC(src, ...) XM_TELEMETRY_DETAIL_EVENT(src, Severity::kTrace, __VA_ARGS__)
#else
#define XM_TRACE_SRC(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 1
#define XM_DEBUG_SRC(src, ...) XM_TELEMETRY_DETAIL_EVENT(src, Severity::kDebug, __VA_ARGS__)
#else
#define XM_DEBUG_SRC(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 2
#define XM_INFO_SRC(src, ...) XM_TELEMETRY_DETAIL_EVENT(src, Severity::kInfo, __VA_ARGS__)
#else
#define XM_INFO_SRC(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 3
#define XM_WARN_SRC(src, ...) XM_TELEMETRY_DETAIL_EVENT(src, Severity::kWarn, __VA_ARGS__)
#else
#define XM_WARN_SRC(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 4
#define XM_ERROR_SRC(src, ...) XM_TELEMETRY_DETAIL_EVENT(src, Severity::kError, __VA_ARGS__)
#else
#define XM_ERROR_SRC(...) XM_TELEMETRY_DETAIL_NOOP()
#endif
#if XM_TELEMETRY_LEVEL <= 5
#define XM_FATAL_SRC(src, ...) XM_TELEMETRY_DETAIL_EVENT(src, Severity::kFatal, __VA_ARGS__)
#else
#define XM_FATAL_SRC(...) XM_TELEMETRY_DETAIL_NOOP()
#endif

// Generic form: XM_EVENT(severity-expression, fmt, ...). Takes any Severity
// expression (e.g. tel::Severity::kWarn, possibly computed at runtime); the
// compile-out floor cannot strip this form, so the shorthand macros above are
// the preferred spelling at call sites.
#define XM_EVENT(sev, ...) \
  ::xmotion::telemetry::detail::EmitEvent(0u, "", (sev), __VA_ARGS__)
