/*
 * telemetry_logging_binding.cpp
 *
 * The INTERIM default binding (ADR 0004 §7, pulled forward to P0a): exposes
 * the existing spdlog-backed DefaultLogger as a telemetry Binding so that the
 * unified event funnel — XM_* and the re-pointed XLOG_* alike — keeps today's
 * logging behavior (levels, async sink, env-var config, log files) with zero
 * init calls. There is ONE logging API; this file is merely its current
 * backend.
 *
 * Transitional by design: the xmTelemetry SDK replaces this wholesale at P0b
 * (Init() installs the real binding; this one is then never consulted), and
 * the spdlog machinery migrates out of xmBase at P1. Deliberately simple:
 *  - events: args are reconstructed into a fmt dynamic_format_arg_store so
 *    format-spec fidelity ({:.3f}, {:x}, ...) is preserved exactly;
 *  - metrics: slots from a process-lifetime arena (aggregate correctly; not
 *    exported anywhere yet — no drain exists until the SDK);
 *  - spans/signals: dropped (recording plane arrives with the SDK);
 *  - health: logged at a level mapped from the state.
 *
 * The arena leaks on exit BY DESIGN: slot memory must be process-lifetime
 * (binding.hpp contract) and never torn down under late static-destructor
 * emits (the spdlog #2113 crash class, scenario S7).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmbase/telemetry/binding.hpp"

#ifdef ENABLE_LOGGING

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "logging/default_logger.hpp"
#if defined(SPDLOG_FMT_EXTERNAL)
#include <fmt/args.h>
#else
#include <spdlog/fmt/bundled/args.h>
#endif

namespace xmotion {
namespace telemetry {
namespace detail {
namespace {

LogLevel ToLogLevel(Severity s) noexcept {
  return static_cast<LogLevel>(s);  // orderings verified identical (0..5)
}

// Process-lifetime registries. std::deque: stable addresses across growth.
struct Arena {
  std::mutex mtx;
  std::deque<CounterSlot> counters;
  std::deque<GaugeSlot> gauges;
  std::deque<HistogramSlot> histograms;
  std::deque<SignalSlot> signals;
  std::unordered_map<std::string, CounterSlot*> counter_ix;
  std::unordered_map<std::string, GaugeSlot*> gauge_ix;
  std::unordered_map<std::string, HistogramSlot*> histogram_ix;
  std::unordered_map<std::string, SignalSlot*> signal_ix;
  std::vector<std::string> source_names;  // index = source id - 1
  std::unordered_map<std::string, std::uint32_t> source_ix;
};

Arena& arena() {
  static Arena* a = new Arena;  // intentional leak — see file header
  return *a;
}

template <typename Slot, typename Deque, typename Index>
Slot* GetSlot(Deque& store, Index& index, std::string_view name) {
  Arena& a = arena();
  std::lock_guard<std::mutex> lock(a.mtx);
  const std::string key(name);
  auto it = index.find(key);
  if (it != index.end()) return it->second;
  store.emplace_back();
  index.emplace(key, &store.back());
  return &store.back();
}

CounterSlot* GetCounterImpl(std::string_view name) {
  return GetSlot<CounterSlot>(arena().counters, arena().counter_ix, name);
}
GaugeSlot* GetGaugeImpl(std::string_view name) {
  return GetSlot<GaugeSlot>(arena().gauges, arena().gauge_ix, name);
}
HistogramSlot* GetHistogramImpl(std::string_view name) {
  return GetSlot<HistogramSlot>(arena().histograms, arena().histogram_ix,
                                name);
}
SignalSlot* GetSignalImpl(std::string_view name, std::size_t, std::size_t,
                          const char*) {
  return GetSlot<SignalSlot>(arena().signals, arena().signal_ix, name);
}

std::uint32_t InternSourceImpl(std::string_view name) {
  Arena& a = arena();
  std::lock_guard<std::mutex> lock(a.mtx);
  const std::string key(name);
  auto it = a.source_ix.find(key);
  if (it != a.source_ix.end()) return it->second;
  a.source_names.push_back(key);
  const auto id = static_cast<std::uint32_t>(a.source_names.size());  // 1-based
  a.source_ix.emplace(key, id);
  return id;
}

// Reconstruct typed fmt arguments from the ArgPack — full spec fidelity.
void BuildArgStore(fmt::dynamic_format_arg_store<fmt::format_context>& store,
                   const ArgPack& p) {
  for (std::uint8_t i = 0; i < p.count; ++i) {
    const unsigned char* src = p.buf + p.offsets[i];
    switch (p.types[i]) {
      case ArgType::kBool: {
        bool v;
        std::memcpy(&v, src, sizeof v);
        store.push_back(v);
        break;
      }
      case ArgType::kI64: {
        std::int64_t v;
        std::memcpy(&v, src, sizeof v);
        store.push_back(v);
        break;
      }
      case ArgType::kU64: {
        std::uint64_t v;
        std::memcpy(&v, src, sizeof v);
        store.push_back(v);
        break;
      }
      case ArgType::kF64: {
        double v;
        std::memcpy(&v, src, sizeof v);
        store.push_back(v);
        break;
      }
      case ArgType::kStr:
        store.push_back(fmt::string_view(
            reinterpret_cast<const char*>(src), p.lens[i]));
        break;
    }
  }
}

void SetLevelImpl(Severity min_sev) noexcept {
  try {
    DefaultLogger::GetInstance().SetLoggerLevel(ToLogLevel(min_sev));
  } catch (...) {
  }
}

Severity GetLevelImpl() noexcept {
  try {
    return static_cast<Severity>(DefaultLogger::GetInstance().GetLoggerLevel());
  } catch (...) {
    return Severity::kWarn;
  }
}

bool ShouldLogImpl(Severity sev) noexcept {
  try {
    return DefaultLogger::GetInstance().ShouldLog(ToLogLevel(sev));
  } catch (...) {
    return sev >= Severity::kWarn;
  }
}

void LogMessage(std::uint32_t source_id, Severity sev, std::string_view msg) {
  auto& logger = DefaultLogger::GetInstance();
  if (source_id != 0) {
    std::string source;
    {
      Arena& a = arena();
      std::lock_guard<std::mutex> lock(a.mtx);
      if (source_id <= a.source_names.size())
        source = a.source_names[source_id - 1];
    }
    if (!source.empty()) {
      logger.Log(ToLogLevel(sev), "[{}] {}", source, msg);
      return;
    }
  }
  logger.Log(ToLogLevel(sev), "{}", msg);
}

void EmitEventImpl(std::uint32_t source_id, Severity sev, const char* fmt_str,
                   const ArgPack& args, Context, Timestamp) noexcept {
  try {
    fmt::dynamic_format_arg_store<fmt::format_context> store;
    BuildArgStore(store, args);
    std::string msg;
    try {
      msg = fmt::vformat(fmt_str, store);
    } catch (...) {
      msg = fmt_str;  // bad spec/arity: log the raw format, never throw
    }
    LogMessage(source_id, sev, msg);
  } catch (...) {
    // logging must never propagate — swallowed by contract (noexcept seam)
  }
}

void EmitEventDynImpl(std::uint32_t source_id, Severity sev, const char* msg,
                      std::size_t len, Context, Timestamp) noexcept {
  try {
    LogMessage(source_id, sev, std::string_view(msg, len));
  } catch (...) {
  }
}

void EmitSpanImpl(const char*, Context, SpanId, Timestamp, Timestamp,
                  const Context*, std::uint8_t) noexcept {
  // recording plane arrives with the SDK; spans are dropped in the interim
}

void EmitSignalImpl(SignalSlot*, const void*, std::size_t,
                    Timestamp) noexcept {
  // ditto
}

void ReportHealthImpl(const char* subsystem, HealthState state,
                      const char* detail_msg, Context, Timestamp) noexcept {
  try {
    const Severity sev = state == HealthState::kOk         ? Severity::kInfo
                         : state == HealthState::kDegraded ? Severity::kWarn
                                                           : Severity::kError;
    const char* name = state == HealthState::kOk         ? "OK"
                       : state == HealthState::kDegraded ? "DEGRADED"
                       : state == HealthState::kFault    ? "FAULT"
                                                         : "DISCONNECTED";
    DefaultLogger::GetInstance().Log(
        ToLogLevel(sev), "[health] {} -> {}{}{}", subsystem, name,
        (detail_msg != nullptr && detail_msg[0] != '\0') ? ": " : "",
        detail_msg != nullptr ? detail_msg : "");
  } catch (...) {
  }
}

void SetResourceImpl(std::string_view, std::string_view) {}

}  // namespace

const Binding* DefaultLoggingBinding() noexcept {
  static const Binding binding = [] {
    Binding b{};
    b.abi_version = kBindingAbiVersion;
    b.get_counter = &GetCounterImpl;
    b.get_gauge = &GetGaugeImpl;
    b.get_histogram = &GetHistogramImpl;
    b.get_signal = &GetSignalImpl;
    b.intern_source = &InternSourceImpl;
    b.should_log = &ShouldLogImpl;
    b.set_level = &SetLevelImpl;
    b.get_level = &GetLevelImpl;
    b.emit_event = &EmitEventImpl;
    b.emit_event_dyn = &EmitEventDynImpl;
    b.emit_span = &EmitSpanImpl;
    b.emit_signal = &EmitSignalImpl;
    b.report_health = &ReportHealthImpl;
    b.set_resource = &SetResourceImpl;
    return b;
  }();
  return &binding;
}

}  // namespace detail
}  // namespace telemetry
}  // namespace xmotion

#else  // !ENABLE_LOGGING — no interim backend; the true unbound state applies.

namespace xmotion {
namespace telemetry {
namespace detail {
const Binding* DefaultLoggingBinding() noexcept { return nullptr; }
}  // namespace detail
}  // namespace telemetry
}  // namespace xmotion

#endif  // ENABLE_LOGGING
