/*
 * telemetry/binding.hpp
 *
 * The API↔SDK seam (ADR 0004 §3): an install-once table of function pointers
 * plus the slot layouts that metric handles point at. The API tier owns the
 * TYPES; the SDK (xmTelemetry) owns the STATE and the THREADS.
 *
 * Binding model:
 *  - unbound (no SDK): the atomic binding pointer is null. Handles point at
 *    shared no-op slots; event()/health() fall back to stderr for Warn+/faults
 *    (implemented in src/telemetry_unbound.cpp); everything else no-ops.
 *  - SDK linked, before Init(): the SDK MAY install a minimal pre-init
 *    binding from a static initializer (bounded buffer, no threads) so early
 *    bring-up events are kept — the delta-D9 contract. Statics that run
 *    before the SDK's own initializer get the unbound behavior; that is the
 *    documented cost of static-init-order being unknowable.
 *  - Init(): the SDK swaps in the full binding (and flushes any pre-init
 *    buffer, in order, with original timestamps).
 *  - Shutdown(): swaps back to null → unbound behavior. The SDK must keep
 *    the outgoing table's functions callable until the swap is globally
 *    visible, and must NEVER free metric slots (slot memory is
 *    process-lifetime by contract) — that is what makes a handle held across
 *    Shutdown() safe (scenario S7-A2).
 *
 * ABI: kBindingAbiVersion gates layout changes; the SDK refuses to install
 * against a mismatched API tier.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/time.hpp"

namespace xmotion {
namespace telemetry {

inline constexpr std::uint32_t kBindingAbiVersion = 3;  // 3: histogram buckets
                                                        // 2: span links (D7)

// Max span links carried inline by a Span (OTel span-link analogue): causal
// associations to OTHER contexts (e.g. the inputs a planning stage consumed)
// without reparenting. Links beyond the cap are dropped silently — links are
// auxiliary metadata, never control flow.
inline constexpr std::uint8_t kMaxSpanLinks = 4;

// Severity shared by events and the unbound fallback threshold. kOff is a
// FILTER value only (SetLogLevel(kOff) silences everything) — never an emit
// severity.
enum class Severity : std::uint8_t {
  kTrace = 0, kDebug = 1, kInfo = 2, kWarn = 3, kError = 4, kFatal = 5,
  kOff = 6,
};

enum class HealthState : std::uint8_t {
  kOk = 0, kDegraded = 1, kFault = 2, kDisconnected = 3,
};

// QoS classes — separate rings per class so a signal flood can never evict a
// diagnostic (ADR 0004 §5). Exposed here because drop accounting is per class.
enum class QosClass : std::uint8_t { kDiagnostics = 0, kSignal = 1 };

namespace detail {

// ---- metric slots ------------------------------------------------------------
// Handles hold a slot pointer fixed at registration; hot-path ops are inline
// atomics on the slot — no binding lookup per call. Slots are allocated by the
// SDK (process-lifetime) or are the shared no-op slots below (unbound).
// C++17 has no atomic<double>::fetch_add — use CAS loops; contention on a
// single metric is expected to be low (one writer loop per metric in practice).

struct CounterSlot {
  std::atomic<double> value{0.0};
  void Add(double v) noexcept {
    double cur = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(cur, cur + v,
                                        std::memory_order_relaxed)) {
    }
  }
};

struct GaugeSlot {
  std::atomic<double> value{0.0};
  void Set(double v) noexcept { value.store(v, std::memory_order_relaxed); }
};

// Exponential (power-of-two) histogram buckets: zero-config, unit-agnostic,
// percentile-capable. Bucket 0 holds v < 1 (incl. 0/negatives/NaN); bucket i
// in [1, 22] holds 2^(i-1) <= v < 2^i; bucket 23 holds v >= 2^22. BucketOf is
// one std::ilogb — no loops, no locks, RT-safe.
inline constexpr std::size_t kHistogramBuckets = 24;
inline std::size_t HistogramBucketOf(double v) noexcept {
  if (!(v >= 1.0)) return 0;  // also catches NaN (comparison is false)
  const int e = std::ilogb(v);  // floor(log2 v), v>=1 => e>=0
  return static_cast<std::size_t>(e + 1 >= 23 ? 23 : e + 1);
}
// Upper bound of a bucket (for percentile estimation): 2^i for i in [0,22].
inline double HistogramBucketUpperBound(std::size_t i) noexcept {
  return i >= 23 ? std::numeric_limits<double>::infinity()
                 : static_cast<double>(1u << i);
}

// ABI v3 layout: count/sum/min/max + exponential buckets (the drain samples
// these aggregates; ADR 0004 §6).
struct HistogramSlot {
  std::atomic<std::uint64_t> count{0};
  std::atomic<double> sum{0.0};
  // Seeded at the identity elements so first-record init needs no coordination.
  std::atomic<double> min{std::numeric_limits<double>::infinity()};
  std::atomic<double> max{-std::numeric_limits<double>::infinity()};
  std::atomic<std::uint64_t> buckets[kHistogramBuckets] = {};
  void Record(double v) noexcept {
    count.fetch_add(1, std::memory_order_relaxed);
    buckets[HistogramBucketOf(v)].fetch_add(1, std::memory_order_relaxed);
    double c = sum.load(std::memory_order_relaxed);
    while (!sum.compare_exchange_weak(c, c + v, std::memory_order_relaxed)) {
    }
    double m = min.load(std::memory_order_relaxed);
    while (v < m &&
           !min.compare_exchange_weak(m, v, std::memory_order_relaxed)) {
    }
    double x = max.load(std::memory_order_relaxed);
    while (v > x &&
           !max.compare_exchange_weak(x, v, std::memory_order_relaxed)) {
    }
  }
};

// Type-erased signal channel slot; typed SignalChannel<T> publishes through
// the binding with (slot, bytes, size, timestamp).
struct SignalSlot {
  std::uint32_t id = 0;  // SDK-interned channel id
};

// Shared no-op slots for the unbound state. Writes from many threads race on
// these dummies — they are atomics and the values are never read.
inline CounterSlot g_noop_counter;
inline GaugeSlot g_noop_gauge;
inline HistogramSlot g_noop_histogram;
inline SignalSlot g_noop_signal;

// ---- deferred-format argument pack (NanoLog/Quill pattern) -------------------
// The hot path copies ARGUMENTS, never formats. Bounded and stack-only:
// overflow truncates (arg dropped, count says how many made it) — never
// allocates, never throws.

enum class ArgType : std::uint8_t { kBool, kI64, kU64, kF64, kStr };

struct ArgPack {
  static constexpr std::size_t kMaxArgs = 8;
  static constexpr std::size_t kBufBytes = 160;
  std::uint8_t count = 0;
  ArgType types[kMaxArgs] = {};
  std::uint8_t offsets[kMaxArgs] = {};
  std::uint8_t lens[kMaxArgs] = {};  // used by kStr
  std::uint8_t used = 0;
  unsigned char buf[kBufBytes] = {};

  bool PutRaw(ArgType t, const void* p, std::size_t n,
              std::size_t display_len) noexcept {
    if (count >= kMaxArgs || used + n > kBufBytes) return false;
    types[count] = t;
    offsets[count] = used;
    lens[count] = static_cast<std::uint8_t>(display_len);
    std::memcpy(buf + used, p, n);
    used = static_cast<std::uint8_t>(used + n);
    ++count;
    return true;
  }
};

inline void PackStr(ArgPack& p, std::string_view v) noexcept {
  // truncate long strings rather than drop them; len records the kept size
  std::size_t n = v.size();
  if (p.used + n > ArgPack::kBufBytes) n = ArgPack::kBufBytes - p.used;
  if (n > 255) n = 255;  // len field is uint8
  p.PutRaw(ArgType::kStr, v.data(), n, n);
}

template <typename T>
void PackOne(ArgPack& p, const T& v) noexcept {
  using D = std::decay_t<T>;
  if constexpr (std::is_same_v<D, bool>) {
    p.PutRaw(ArgType::kBool, &v, sizeof(bool), 0);
  } else if constexpr (std::is_same_v<D, char*> ||
                       std::is_same_v<D, const char*>) {
    PackStr(p, v == nullptr ? std::string_view("(null)")
                            : std::string_view(v));
  } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
    const std::int64_t w = static_cast<std::int64_t>(v);
    p.PutRaw(ArgType::kI64, &w, sizeof w, 0);
  } else if constexpr (std::is_integral_v<D>) {
    const std::uint64_t w = static_cast<std::uint64_t>(v);
    p.PutRaw(ArgType::kU64, &w, sizeof w, 0);
  } else if constexpr (std::is_floating_point_v<D>) {
    const double w = static_cast<double>(v);
    p.PutRaw(ArgType::kF64, &w, sizeof w, 0);
  } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    PackStr(p, std::string_view(v));
  } else {
    static_assert(std::is_arithmetic_v<D>,
                  "unsupported telemetry event argument type: pass arithmetic "
                  "types, string_view, or const char* (format rich types "
                  "yourself OFF the hot path)");
  }
}

template <typename... Args>
ArgPack PackArgs(Args&&... args) noexcept {
  ArgPack p;
  (PackOne(p, std::forward<Args>(args)), ...);
  return p;
}

}  // namespace detail

// ---- the binding table --------------------------------------------------------

// Installed by the SDK; every pointer must be non-null in an installed table.
// Registration entries may allocate (init-time); emit entries must be
// noexcept, allocation-free, and wait-free (they land in the SDK's rings).
// `name`/`fmt` parameters must be string literals (static storage) — the SDK
// interns by pointer identity.
struct Binding {
  std::uint32_t abi_version = kBindingAbiVersion;

  // registration (non-RT; called from Get* / RegisterEventSource)
  detail::CounterSlot* (*get_counter)(std::string_view name);
  detail::GaugeSlot* (*get_gauge)(std::string_view name);
  detail::HistogramSlot* (*get_histogram)(std::string_view name);
  detail::SignalSlot* (*get_signal)(std::string_view name,
                                    std::size_t payload_size,
                                    std::size_t payload_align,
                                    const char* payload_type_name);
  std::uint32_t (*intern_source)(std::string_view name);  // EventSource id

  // hot path (RT-safe by contract)
  // Runtime level gate: lets call sites (incl. the XM_*_STREAM macros) skip
  // argument packing / string building for suppressed severities.
  bool (*should_log)(Severity sev) noexcept;
  // Runtime minimum-severity control (SetLogLevel/GetLogLevel).
  void (*set_level)(Severity min_sev) noexcept;
  Severity (*get_level)() noexcept;
  void (*emit_event)(std::uint32_t source_id, Severity sev, const char* fmt,
                     const detail::ArgPack& args, Context ctx,
                     Timestamp ts) noexcept;
  // Pre-formatted / dynamic-string events (the XM_*_STREAM path and other
  // non-literal messages): `msg` need NOT be a string literal — the SDK must
  // copy it before returning. Non-RT convenience.
  void (*emit_event_dyn)(std::uint32_t source_id, Severity sev,
                         const char* msg, std::size_t len, Context ctx,
                         Timestamp ts) noexcept;
  // `links` points at up to kMaxSpanLinks contexts causally associated with
  // this span (D7); valid only for the duration of the call — the SDK copies.
  void (*emit_span)(const char* name, Context ctx, SpanId parent,
                    Timestamp begin, Timestamp end, const Context* links,
                    std::uint8_t link_count) noexcept;
  void (*emit_signal)(detail::SignalSlot* slot, const void* bytes,
                      std::size_t size, Timestamp ts) noexcept;
  void (*report_health)(const char* subsystem, HealthState state,
                        const char* detail, Context ctx,
                        Timestamp ts) noexcept;

  // resource attributes (non-RT, set-once-at-startup)
  void (*set_resource)(std::string_view key, std::string_view value);
};

// Install/replace the active binding (SDK Init/Shutdown). Passing nullptr
// reverts to the unbound behavior. Returns false on ABI mismatch.
bool InstallBinding(const Binding* binding) noexcept;

// The active binding, or nullptr when unbound. One acquire load — the only
// per-call indirection on event/span/signal/health paths (metric handles skip
// even this: their slot pointer was fixed at registration).
const Binding* ActiveBinding() noexcept;

// Unbound fallback used by event.hpp / health.hpp when no binding is active:
// formats with a minimal std-only "{}" formatter and writes synchronously to
// stderr for Warn+ (events) / non-Ok (health). Deliberately simple — the
// unbound state is the degraded mode, not the RT deployment (ADR 0004 §2).
namespace detail {
void UnboundEmitEvent(const char* source, Severity sev, const char* fmt,
                      const ArgPack& args) noexcept;
void UnboundEmitEventDyn(const char* source, Severity sev, const char* msg,
                         std::size_t len) noexcept;
void UnboundReportHealth(const char* subsystem, HealthState state,
                         const char* detail) noexcept;

// The built-in default binding: a dependency-free console sink (full-severity
// stderr lines, spec-subset formatter, runtime level; see
// src/telemetry_console_binding.cpp) compiled into xmBase when
// ENABLE_LOGGING is ON — a lib-only build logs honestly with zero init calls
// until an application installs the xmTelemetry SDK binding. Returns nullptr
// when logging is disabled. ActiveBinding() adopts it lazily unless a binding
// has been EXPLICITLY installed (incl. explicit nullptr — an explicit unbind,
// e.g. SDK Shutdown() or a test, is authoritative and disables auto-adoption
// for the remainder of the process).
const Binding* DefaultConsoleBinding() noexcept;
}  // namespace detail

}  // namespace telemetry
}  // namespace xmotion
