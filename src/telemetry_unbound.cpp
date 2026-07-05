/*
 * telemetry_unbound.cpp
 *
 * The compiled sliver of the telemetry API tier: the atomic binding pointer,
 * the per-thread id generator, and the unbound stderr fallback (a minimal
 * std-only "{}" formatter). Everything else in xmbase/telemetry/ is
 * header-only. Deliberately simple: the unbound state is the degraded mode
 * of a lib-only build, not the RT deployment (ADR 0004 §2).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>

#include "xmbase/telemetry/binding.hpp"

namespace xmotion {
namespace telemetry {

namespace {

std::atomic<const Binding*> g_binding{nullptr};
// Set by any EXPLICIT InstallBinding call (incl. nullptr). Once set, the
// interim logging binding is never auto-adopted again — an explicit unbind
// (SDK Shutdown, or a test pinning the true-unbound state) is authoritative.
std::atomic<bool> g_explicitly_set{false};

const char* SeverityName(Severity s) {
  switch (s) {
    case Severity::kTrace: return "TRACE";
    case Severity::kDebug: return "DEBUG";
    case Severity::kInfo: return "INFO";
    case Severity::kWarn: return "WARN";
    case Severity::kError: return "ERROR";
    case Severity::kFatal: return "FATAL";
    case Severity::kOff: break;  // filter value, never emitted
  }
  return "?";
}

const char* HealthName(HealthState s) {
  switch (s) {
    case HealthState::kOk: return "OK";
    case HealthState::kDegraded: return "DEGRADED";
    case HealthState::kFault: return "FAULT";
    case HealthState::kDisconnected: return "DISCONNECTED";
  }
  return "?";
}

// Append one packed argument's default rendering to out (bounded).
void AppendArg(char* out, std::size_t cap, std::size_t& len,
               const detail::ArgPack& p, std::uint8_t i) {
  char tmp[64];
  int n = 0;
  const unsigned char* src = p.buf + p.offsets[i];
  switch (p.types[i]) {
    case detail::ArgType::kBool: {
      bool v;
      std::memcpy(&v, src, sizeof v);
      n = std::snprintf(tmp, sizeof tmp, "%s", v ? "true" : "false");
      break;
    }
    case detail::ArgType::kI64: {
      std::int64_t v;
      std::memcpy(&v, src, sizeof v);
      n = std::snprintf(tmp, sizeof tmp, "%" PRId64, v);
      break;
    }
    case detail::ArgType::kU64: {
      std::uint64_t v;
      std::memcpy(&v, src, sizeof v);
      n = std::snprintf(tmp, sizeof tmp, "%" PRIu64, v);
      break;
    }
    case detail::ArgType::kF64: {
      double v;
      std::memcpy(&v, src, sizeof v);
      n = std::snprintf(tmp, sizeof tmp, "%g", v);
      break;
    }
    case detail::ArgType::kStr: {
      const std::size_t sl = p.lens[i];
      for (std::size_t k = 0; k < sl && len < cap - 1; ++k)
        out[len++] = static_cast<char>(src[k]);
      return;
    }
  }
  for (int k = 0; k < n && len < cap - 1; ++k) out[len++] = tmp[k];
}

// Minimal deferred formatter: each "{...}" placeholder consumes the next
// packed argument with its default rendering (format specs inside the braces
// are ignored — the SDK's real formatter handles them; this is stderr triage).
void FormatInto(char* out, std::size_t cap, const char* fmt,
                const detail::ArgPack& args) {
  std::size_t len = 0;
  std::uint8_t next = 0;
  for (const char* c = fmt; *c != '\0' && len < cap - 1; ++c) {
    if (c[0] == '{' && c[1] == '{') {  // escaped brace
      out[len++] = '{';
      ++c;
    } else if (c[0] == '}' && c[1] == '}') {  // escaped closing brace
      out[len++] = '}';
      ++c;
    } else if (*c == '{') {
      while (*c != '\0' && *c != '}') ++c;  // skip to closing brace
      if (*c == '\0') break;
      if (next < args.count) {
        AppendArg(out, cap, len, args, next++);
      } else {
        const char* miss = "{?}";
        for (const char* m = miss; *m != '\0' && len < cap - 1; ++m)
          out[len++] = *m;
      }
    } else {
      out[len++] = *c;
    }
  }
  out[len] = '\0';
}

}  // namespace

bool InstallBinding(const Binding* binding) noexcept {
  if (binding != nullptr && binding->abi_version != kBindingAbiVersion)
    return false;
  g_explicitly_set.store(true, std::memory_order_relaxed);
  g_binding.store(binding, std::memory_order_release);
  return true;
}

const Binding* ActiveBinding() noexcept {
  const Binding* b = g_binding.load(std::memory_order_acquire);
  if (b == nullptr && !g_explicitly_set.load(std::memory_order_relaxed)) {
    // Lazily adopt the built-in console binding: XM_* logs to stderr with
    // zero init calls until the xmTelemetry SDK installs the real machinery.
    // CAS so a concurrent explicit InstallBinding wins.
    const Binding* def = detail::DefaultConsoleBinding();
    if (def != nullptr) {
      const Binding* expected = nullptr;
      g_binding.compare_exchange_strong(expected, def,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
      b = g_binding.load(std::memory_order_acquire);
    }
  }
  return b;
}

namespace detail {

std::uint64_t NextId() noexcept {
  // splitmix64 over per-thread state, seeded lazily from random_device.
  thread_local std::uint64_t state = [] {
    std::random_device rd;
    std::uint64_t s = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    return s != 0 ? s : 0x9E3779B97F4A7C15ull;
  }();
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z = z ^ (z >> 31);
  return z != 0 ? z : 1;  // ids are nonzero by contract
}

void UnboundEmitEvent(const char* source, Severity sev, const char* fmt,
                      const ArgPack& args) noexcept {
  char msg[512];
  FormatInto(msg, sizeof msg, fmt, args);
  if (source != nullptr && source[0] != '\0') {
    std::fprintf(stderr, "[xmtelemetry:%s] [%s] %s\n", SeverityName(sev),
                 source, msg);
  } else {
    std::fprintf(stderr, "[xmtelemetry:%s] %s\n", SeverityName(sev), msg);
  }
}

void UnboundEmitEventDyn(const char* source, Severity sev, const char* msg,
                         std::size_t len) noexcept {
  const int n = len > 480 ? 480 : static_cast<int>(len);
  if (source != nullptr && source[0] != '\0') {
    std::fprintf(stderr, "[xmtelemetry:%s] [%s] %.*s\n", SeverityName(sev),
                 source, n, msg);
  } else {
    std::fprintf(stderr, "[xmtelemetry:%s] %.*s\n", SeverityName(sev), n, msg);
  }
}

void UnboundReportHealth(const char* subsystem, HealthState state,
                         const char* detail_msg) noexcept {
  std::fprintf(stderr, "[xmtelemetry:HEALTH] %s -> %s%s%s\n",
               subsystem != nullptr ? subsystem : "?", HealthName(state),
               (detail_msg != nullptr && detail_msg[0] != '\0') ? ": " : "",
               detail_msg != nullptr ? detail_msg : "");
}

}  // namespace detail

}  // namespace telemetry
}  // namespace xmotion
