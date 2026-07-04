/*
 * telemetry/handles.hpp
 *
 * Pre-registered instrument handles (design doc §5.2). Handles are small
 * VALUE types wrapping a slot pointer fixed at registration (delta D14):
 * trivially copyable, no lifetime coupling to any registry, and safe to hold
 * across SDK Shutdown() (slot memory is process-lifetime by the seam
 * contract). Acquire handles at init; use them on the hot path.
 *
 * Contract for handles acquired while UNBOUND (no SDK / before its binding
 * is installed): they point at shared no-op slots permanently — they do not
 * retroactively upgrade when the SDK binds. Registration belongs in
 * init-time code, after telemetry::Init() (D9 discussion in binding.hpp).
 *
 * Metric semantics (ADR 0004 §6): Add/Set/Record are relaxed atomics on the
 * slot — no ring traffic, no binding lookup, no allocation; the SDK's drain
 * samples the aggregates.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <string_view>
#include <type_traits>
#include <typeinfo>

#include "xmbase/telemetry/binding.hpp"
#include "xmbase/telemetry/time.hpp"

namespace xmotion {
namespace telemetry {

class Counter {
 public:
  void Add(double v = 1.0) noexcept { slot_->Add(v); }

 private:
  friend Counter GetCounter(std::string_view) noexcept;
  explicit Counter(detail::CounterSlot* s) noexcept : slot_(s) {}
  detail::CounterSlot* slot_;
};

class Gauge {
 public:
  void Set(double v) noexcept { slot_->Set(v); }

 private:
  friend Gauge GetGauge(std::string_view) noexcept;
  explicit Gauge(detail::GaugeSlot* s) noexcept : slot_(s) {}
  detail::GaugeSlot* slot_;
};

class Histogram {
 public:
  void Record(double v) noexcept { slot_->Record(v); }

 private:
  friend Histogram GetHistogram(std::string_view) noexcept;
  explicit Histogram(detail::HistogramSlot* s) noexcept : slot_(s) {}
  detail::HistogramSlot* slot_;
};

inline Counter GetCounter(std::string_view name) noexcept {
  const Binding* b = ActiveBinding();
  return Counter(b != nullptr ? b->get_counter(name) : &detail::g_noop_counter);
}

inline Gauge GetGauge(std::string_view name) noexcept {
  const Binding* b = ActiveBinding();
  return Gauge(b != nullptr ? b->get_gauge(name) : &detail::g_noop_gauge);
}

inline Histogram GetHistogram(std::string_view name) noexcept {
  const Binding* b = ActiveBinding();
  return Histogram(b != nullptr ? b->get_histogram(name)
                                : &detail::g_noop_histogram);
}

// ---- high-rate typed signals (the recording plane's front door) -------------

inline constexpr std::size_t kMaxSignalPayload = 192;  // delta D4/D5: records
// are fixed-size; larger data decomposes into per-sample records (a
// trajectory publishes points, not a blob — see scenarios.md D5).

template <typename T>
class SignalChannel {
  static_assert(std::is_trivially_copyable_v<T>,
                "signal payloads must be trivially copyable PODs (D4)");
  static_assert(sizeof(T) <= kMaxSignalPayload,
                "signal payload exceeds kMaxSignalPayload — decompose into "
                "per-sample records (D5)");

 public:
  void Publish(const T& sample, Timestamp t = Now()) noexcept {
    const Binding* b = ActiveBinding();
    if (b != nullptr) b->emit_signal(slot_, &sample, sizeof(T), t);
  }

 private:
  template <typename U>
  friend SignalChannel<U> GetChannel(std::string_view) noexcept;
  explicit SignalChannel(detail::SignalSlot* s) noexcept : slot_(s) {}
  detail::SignalSlot* slot_;
};

template <typename T>
SignalChannel<T> GetChannel(std::string_view name) noexcept {
  const Binding* b = ActiveBinding();
  if (b == nullptr) return SignalChannel<T>(&detail::g_noop_signal);
  // Best-effort schema: name + size + alignment + compiler type name. The
  // self-describing schema story (MCAP channels) is finalized at P3.
  return SignalChannel<T>(
      b->get_signal(name, sizeof(T), alignof(T), typeid(T).name()));
}

}  // namespace telemetry
}  // namespace xmotion
