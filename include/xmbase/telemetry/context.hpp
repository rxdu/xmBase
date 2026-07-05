/*
 * telemetry/context.hpp
 *
 * Correlation identity — the coherence spine (ADR 0004). W3C-shaped ids; the
 * library owns the FORMAT, the application owns the CARRIER (a message header
 * field, a DDS envelope, a queue struct): Inject()/Extract() are the seam.
 *
 * The thread-local current context is the only state the API tier holds; it
 * is a POD variable, not machinery, and works identically bound and unbound.
 *
 * Names/format strings passed to telemetry must be string literals (static
 * storage duration) — the SDK keys interning off the pointer.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace xmotion {
namespace telemetry {

// 128-bit trace id + 64-bit span id (W3C trace-context shaped).
struct TraceId {
  std::uint64_t hi = 0, lo = 0;
  bool valid() const noexcept { return (hi | lo) != 0; }
  friend bool operator==(const TraceId& a, const TraceId& b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
  }
};

struct SpanId {
  std::uint64_t value = 0;
  bool valid() const noexcept { return value != 0; }
  friend bool operator==(const SpanId& a, const SpanId& b) noexcept {
    return a.value == b.value;
  }
};

// The propagated pair. A zero Context means "none".
struct Context {
  TraceId trace;
  SpanId span;
  bool valid() const noexcept { return trace.valid(); }
};

namespace detail {
// The one piece of API-tier state: the calling thread's current context.
inline thread_local Context t_current_context{};

// Cheap per-thread id generator: splitmix64 over a per-thread state seeded
// once from std::random_device (lazily, NOT on the hot path after first use).
std::uint64_t NextId() noexcept;  // never returns 0
}  // namespace detail

// ---- current context (thread-local) -----------------------------------------

inline Context CurrentContext() noexcept { return detail::t_current_context; }
inline void SetCurrentContext(Context ctx) noexcept {
  detail::t_current_context = ctx;
}

// RAII set-and-restore (delta D2): message-driven handlers MUST use this (or
// Span) rather than bare SetCurrentContext, or the context leaks into the
// next unrelated task on the same thread (the pool-pollution failure mode,
// scenario S2-A5).
class ContextGuard {
 public:
  explicit ContextGuard(Context ctx) noexcept
      : saved_(detail::t_current_context) {
    detail::t_current_context = ctx;
  }
  ~ContextGuard() { detail::t_current_context = saved_; }
  ContextGuard(const ContextGuard&) = delete;
  ContextGuard& operator=(const ContextGuard&) = delete;

 private:
  Context saved_;
};

// ---- trace creation (delta D1) ----------------------------------------------

// Mint a fresh root context (new 128-bit trace id + root span id). First call
// on a thread seeds the id generator (may touch std::random_device) — mint
// traces at ingress/init, not deep inside a hard-RT loop.
inline Context NewTrace() noexcept {
  Context ctx;
  ctx.trace.hi = detail::NextId();
  ctx.trace.lo = detail::NextId();
  ctx.span.value = detail::NextId();
  return ctx;
}

// ---- envelope carriage (report §10.5) ---------------------------------------

inline constexpr std::size_t kContextWireSize = 24;  // trace.hi|trace.lo|span

// Serialize for a message envelope. Byte order is the host's — envelopes are
// intra-robot; converting to W3C `traceparent` text is bridge-layer work.
inline std::array<std::uint8_t, kContextWireSize> Inject(Context ctx) noexcept {
  std::array<std::uint8_t, kContextWireSize> out;
  std::memcpy(out.data(), &ctx.trace.hi, 8);
  std::memcpy(out.data() + 8, &ctx.trace.lo, 8);
  std::memcpy(out.data() + 16, &ctx.span.value, 8);
  return out;
}

// Parse from an envelope. Short/invalid input yields a zero ("none") Context —
// weak validation at the boundary is a family anti-pattern, so check valid().
inline Context Extract(const std::uint8_t* bytes, std::size_t len) noexcept {
  Context ctx;
  if (bytes == nullptr || len < kContextWireSize) return ctx;
  std::memcpy(&ctx.trace.hi, bytes, 8);
  std::memcpy(&ctx.trace.lo, bytes + 8, 8);
  std::memcpy(&ctx.span.value, bytes + 16, 8);
  return ctx;
}

}  // namespace telemetry
}  // namespace xmotion
