/*
 * custom_binding_example.cpp
 *
 * REFERENCE USAGE — the BLUEPRINT made executable: a minimal custom Binding
 * (~100 lines, no dependencies) that captures EVERY verb — events, spans WITH
 * links, high-rate signals, health, and metric aggregates — and prints them,
 * demonstrating the full data flow behind the seam (reference.md) without
 * the xmTelemetry SDK. This is also the pattern for special backends (tests,
 * bridges, bare-metal-ish targets).
 *
 * Contract highlights (binding.hpp): emit_* are noexcept and must not block;
 * slot memory is process-lifetime (static here); pointer args are valid only
 * for the call. A real backend typically captures asynchronously and processes on its own
 * thread — this demo prints inline for clarity, which is NOT RT-safe.
 *
 * Also demonstrates the D5 pattern: a trajectory publishes per-sample POD
 * points, never a variable-length blob.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdio>
#include <tuple>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;

namespace {

// ---- a tiny backend: print-everything binding --------------------------------

struct MiniBackend {
  // Process-lifetime registries (seam contract: slots are never freed).
  static std::mutex& mtx() { static std::mutex m; return m; }
  static std::deque<std::pair<std::string, tel::detail::CounterSlot>>& counters() {
    static auto* d = new std::deque<std::pair<std::string, tel::detail::CounterSlot>>();
    return *d;
  }
  static std::deque<std::pair<std::string, tel::detail::GaugeSlot>>& gauges() {
    static auto* d = new std::deque<std::pair<std::string, tel::detail::GaugeSlot>>();
    return *d;
  }

  // Slots hold atomics (non-movable) — construct them in place.
  template <typename D>
  static auto* Slot(D& store, std::string_view name) {
    std::lock_guard<std::mutex> lk(mtx());
    for (auto& [n, slot] : store)
      if (n == name) return &slot;
    store.emplace_back(std::piecewise_construct,
                       std::forward_as_tuple(name), std::forward_as_tuple());
    return &store.back().second;
  }

  static const tel::Binding* Make() {
    static tel::detail::HistogramSlot histo;   // shared demo slots for brevity
    static tel::detail::SignalSlot signal;
    static tel::Binding b = [] {
      tel::Binding x{};
      x.abi_version = tel::kBindingAbiVersion;
      x.get_counter = [](std::string_view n) { return Slot(counters(), n); };
      x.get_gauge = [](std::string_view n) { return Slot(gauges(), n); };
      x.get_histogram = [](std::string_view) { return &histo; };
      x.get_signal = [](std::string_view n, std::size_t sz, std::size_t,
                        const char*) {
        std::printf("[mini] channel registered: %.*s (%zu B/sample)\n",
                    int(n.size()), n.data(), sz);
        return &signal;
      };
      x.intern_source = [](std::string_view) { return 1u; };
      x.should_log = [](tel::Severity) noexcept { return true; };
      x.set_level = [](tel::Severity) noexcept {};
      x.get_level = []() noexcept { return tel::Severity::kTrace; };
      x.emit_event = [](std::uint32_t src, tel::Severity sev, const char* fmt,
                        const tel::detail::ArgPack& args, tel::Context,
                        tel::Timestamp) noexcept {
        std::printf("[mini] event  sev=%d src=%u fmt=\"%s\" (%d args)\n",
                    int(sev), src, fmt, int(args.count));
      };
      x.emit_event_dyn = [](std::uint32_t, tel::Severity sev, const char* msg,
                            std::size_t len, tel::Context,
                            tel::Timestamp) noexcept {
        std::printf("[mini] event* sev=%d \"%.*s\"\n", int(sev), int(len), msg);
      };
      x.emit_span = [](const char* name, tel::Context ctx, tel::SpanId,
                       tel::Timestamp t0, tel::Timestamp t1,
                       const tel::Context*, std::uint8_t links) noexcept {
        std::printf("[mini] span   %-22s %6.2f ms  links=%u  trace=%016llx…\n",
                    name,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    unsigned(links),
                    static_cast<unsigned long long>(ctx.trace.hi));
      };
      x.emit_signal = [](tel::detail::SignalSlot*, const void*,
                         std::size_t size, tel::Timestamp) noexcept {
        std::printf("[mini] signal %zu B\n", size);
      };
      x.report_health = [](const char* sub, tel::HealthState s, const char* d,
                           tel::Context, tel::Timestamp) noexcept {
        std::printf("[mini] health %s -> %d (%s)\n", sub, int(s), d);
      };
      x.set_resource = [](std::string_view k, std::string_view v) {
        std::printf("[mini] resource %.*s=%.*s\n", int(k.size()), k.data(),
                    int(v.size()), v.data());
      };
      return x;
    }();
    return &b;
  }
};

// D5 pattern: per-sample trajectory point — never a variable-length blob.
struct TrajPoint {
  std::uint32_t index;
  double x, y, v;
};

}  // namespace

int main() {
  // The application (and ONLY the application) installs the backend.
  if (!tel::InstallBinding(MiniBackend::Make())) {
    std::fprintf(stderr, "ABI mismatch\n");
    return 1;
  }
  tel::SetResource("robot.id", "demo-bot");

  // Every verb, now visibly captured:
  auto cycles = tel::GetCounter("demo.cycles");
  auto traj = tel::GetChannel<TrajPoint>("demo.plan.traj_point");

  const tel::Context upstream = tel::NewTrace();
  {
    tel::ContextGuard g(tel::NewTrace());
    tel::Span plan("demo.plan.iteration");
    plan.AddLink(upstream);                       // span LINK (D7)
    {
      XM_SPAN("demo.plan.generate");
      for (std::uint32_t i = 0; i < 3; ++i) {     // D5: per-sample publish
        traj.Publish(TrajPoint{i, 0.1 * i, 0.2 * i, 1.0});
        cycles.Add();
      }
      XM_INFO("trajectory ready: {} points", 3);
      XM_WARN_STREAM("streamed " << 42);
    }
  }
  tel::ReportHealth("demo.planner", tel::HealthState::kOk, "nominal");

  // Metric aggregates live in the slots the backend allocated — read at exit
  // (a real backend samples these periodically instead).
  for (auto& [name, slot] : MiniBackend::counters())
    std::printf("[mini] metric %s = %.0f\n", name.c_str(),
                slot.value.load(std::memory_order_relaxed));

  tel::InstallBinding(nullptr);  // explicit unbind — authoritative (see docs)
  return 0;
}
