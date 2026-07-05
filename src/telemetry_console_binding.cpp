/*
 * telemetry_console_binding.cpp
 *
 * The PERMANENT default binding: a dependency-free (libc/libstdc++ only)
 * full-severity console sink. xmBase alone — no SDK, no third-party
 * libraries — gives honest console logging out of the box; the xmTelemetry
 * SDK replaces this wholesale when an application installs its binding
 * (explicit InstallBinding always wins over auto-adoption — see
 * telemetry_unbound.cpp).
 *
 * Design points:
 *  - emit_event formats the deferred ArgPack against the fmt string with a
 *    spec-SUBSET formatter ("{}" plus "{:[0][width][.prec][fFeEgGdxXos]}");
 *    unsupported specs fall back to the argument's default rendering. All
 *    formatting goes through snprintf into stack buffers — no heap
 *    allocation on the emit path; over-long lines truncate gracefully.
 *  - one complete line per write(2) call, so concurrent emits from many
 *    threads never interleave mid-line.
 *  - line shape matches the previous console output for continuity:
 *      [info] [<epoch-sec>.<nanos>] [<process>]: [<source>] <message>
 *    colorized per severity when stderr is a terminal.
 *  - metric/signal slots are shared process-lifetime statics (safe no-ops:
 *    atomically written, never read — same contract as the unbound slots);
 *    spans/signals are dropped (the recording plane is the SDK's job);
 *    health transitions log one line (operator-relevant).
 *  - runtime level: one atomic, default Info, seeded once from $XM_LOG_LEVEL
 *    (0..6) for continuity with the documented environment configuration.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmbase/telemetry/binding.hpp"

#ifdef ENABLE_LOGGING

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace xmotion {
namespace telemetry {
namespace detail {
namespace {

// ---- runtime level ----------------------------------------------------------

// One atomic minimum severity; default Info. Seeded from $XM_LOG_LEVEL (0..6)
// exactly once, when the binding table is first materialized.
std::atomic<std::uint8_t> g_level{
    static_cast<std::uint8_t>(Severity::kInfo)};

Severity LevelFromEnv(Severity fallback) noexcept {
  const char* value = std::getenv("XM_LOG_LEVEL");
  if (value == nullptr || value[0] == '\0') return fallback;
  char* end = nullptr;
  const long level = std::strtol(value, &end, 10);
  if (end == value || level < 0 || level > 6) return fallback;
  return static_cast<Severity>(level);
}

bool ShouldLogImpl(Severity sev) noexcept {
  return sev != Severity::kOff &&
         static_cast<std::uint8_t>(sev) >=
             g_level.load(std::memory_order_relaxed);
}

void SetLevelImpl(Severity min_sev) noexcept {
  g_level.store(static_cast<std::uint8_t>(min_sev),
                std::memory_order_relaxed);
}

Severity GetLevelImpl() noexcept {
  return static_cast<Severity>(g_level.load(std::memory_order_relaxed));
}

// ---- metric / signal slots ----------------------------------------------------

// Shared process-lifetime statics: handles stay valid and writable forever
// (atomic writes, values never read) — the same "safe no-op" contract as the
// unbound slots in binding.hpp. There is no drain in the console binding, so
// per-name aggregation would be state nobody can observe.
CounterSlot g_console_counter;
GaugeSlot g_console_gauge;
HistogramSlot g_console_histogram;
SignalSlot g_console_signal;

CounterSlot* GetCounterImpl(std::string_view) { return &g_console_counter; }
GaugeSlot* GetGaugeImpl(std::string_view) { return &g_console_gauge; }
HistogramSlot* GetHistogramImpl(std::string_view) {
  return &g_console_histogram;
}
SignalSlot* GetSignalImpl(std::string_view, std::size_t, std::size_t,
                          const char*) {
  return &g_console_signal;
}

// ---- source registry ----------------------------------------------------------

// Interned source names. `name` is a string literal by contract (binding.hpp),
// so storing the pointer + length is stable and allocation happens only in the
// vector itself (registration path, non-RT). Leaked by design: ids must stay
// resolvable under late static-destructor emits.
struct SourceEntry {
  const char* data;
  std::size_t len;
};

struct SourceRegistry {
  std::mutex mtx;
  std::vector<SourceEntry> names;  // index = id - 1
};

SourceRegistry& Sources() {
  static SourceRegistry* r = new SourceRegistry;  // intentional leak
  return *r;
}

std::uint32_t InternSourceImpl(std::string_view name) {
  SourceRegistry& r = Sources();
  std::lock_guard<std::mutex> lock(r.mtx);
  for (std::size_t i = 0; i < r.names.size(); ++i) {
    if (std::string_view(r.names[i].data, r.names[i].len) == name)
      return static_cast<std::uint32_t>(i + 1);
  }
  r.names.push_back(SourceEntry{name.data(), name.size()});
  return static_cast<std::uint32_t>(r.names.size());  // 1-based
}

// Resolve id -> (ptr, len); {nullptr, 0} for the anonymous/unknown source.
SourceEntry LookupSource(std::uint32_t id) noexcept {
  if (id == 0) return {nullptr, 0};
  SourceRegistry& r = Sources();
  std::lock_guard<std::mutex> lock(r.mtx);
  if (id > r.names.size()) return {nullptr, 0};
  return r.names[id - 1];
}

// ---- spec-subset formatter ------------------------------------------------------

// Placeholder spec: "{}" or "{:[0][width][.prec][type]}" with type in
// [fFeEgGdxXos]. Anything else (fill/align, '+', '#', positional, named,
// nested) is UNSUPPORTED: the placeholder still consumes the next argument
// and renders it with its default formatting — degraded output, never a
// dropped record.
struct FmtSpec {
  bool ok = false;        // spec parsed within the subset
  bool zero = false;      // '0' flag
  int width = -1;         // -1 = unset
  int prec = -1;          // -1 = unset
  char type = '\0';       // '\0' = default per argument type
};

constexpr int kMaxWidth = 96;  // clamp so a hostile width can't blow the line
constexpr int kMaxPrec = 64;

// Parse the text between ':' and '}' (exclusive). Returns spec with ok=false
// when anything outside the subset appears.
FmtSpec ParseSpec(const char* s, const char* end) noexcept {
  FmtSpec spec;
  if (s < end && *s == '0') {
    spec.zero = true;
    ++s;
  }
  if (s < end && *s >= '0' && *s <= '9') {
    int w = 0;
    while (s < end && *s >= '0' && *s <= '9') {
      w = w * 10 + (*s - '0');
      if (w > kMaxWidth) w = kMaxWidth;
      ++s;
    }
    spec.width = w;
  }
  if (s < end && *s == '.') {
    ++s;
    if (s >= end || *s < '0' || *s > '9') return spec;  // ".": unsupported
    int p = 0;
    while (s < end && *s >= '0' && *s <= '9') {
      p = p * 10 + (*s - '0');
      if (p > kMaxPrec) p = kMaxPrec;
      ++s;
    }
    spec.prec = p;
  }
  if (s < end) {
    if (std::strchr("fFeEgGdxXos", *s) == nullptr) return spec;
    spec.type = *s++;
  }
  spec.ok = (s == end);
  return spec;
}

void AppendRaw(char* out, std::size_t cap, std::size_t& len, const char* s,
               std::size_t n) noexcept {
  for (std::size_t i = 0; i < n && len < cap - 1; ++i) out[len++] = s[i];
}

// Build a printf conversion "%[0][*][.*]<conv>" and render into tmp. `conv`
// is a printf length+conversion suffix ("lld", "llx", "f", "s", ...).
void RenderNumeric(char* out, std::size_t cap, std::size_t& len,
                   const FmtSpec& spec, const char* conv, ...) noexcept {
  char pf[24];
  std::size_t pi = 0;
  pf[pi++] = '%';
  if (spec.zero) pf[pi++] = '0';
  if (spec.width >= 0) pi += static_cast<std::size_t>(std::snprintf(
      pf + pi, sizeof pf - pi, "%d", spec.width));
  if (spec.prec >= 0) pi += static_cast<std::size_t>(std::snprintf(
      pf + pi, sizeof pf - pi, ".%d", spec.prec));
  std::snprintf(pf + pi, sizeof pf - pi, "%s", conv);

  char tmp[192];  // large enough for kMaxWidth/kMaxPrec renderings
  va_list ap;
  va_start(ap, conv);
  const int n = std::vsnprintf(tmp, sizeof tmp, pf, ap);
  va_end(ap);
  if (n > 0)
    AppendRaw(out, cap, len, tmp,
              static_cast<std::size_t>(n) < sizeof tmp
                  ? static_cast<std::size_t>(n)
                  : sizeof tmp - 1);
}

// Render one packed argument honoring the (already parsed) spec. A spec that
// does not apply to the argument's type falls back to default rendering.
void RenderArg(char* out, std::size_t cap, std::size_t& len,
               const ArgPack& p, std::uint8_t i, FmtSpec spec) noexcept {
  const unsigned char* src = p.buf + p.offsets[i];
  switch (p.types[i]) {
    case ArgType::kBool: {
      bool v;
      std::memcpy(&v, src, sizeof v);
      if (spec.ok && spec.prec < 0 &&
          (spec.type == 'd' || spec.type == 'x' || spec.type == 'X' ||
           spec.type == 'o')) {
        const char conv[4] = {'l', 'l', spec.type == 'd' ? 'd' : spec.type,
                              '\0'};
        RenderNumeric(out, cap, len, spec, conv,
                      static_cast<long long>(v ? 1 : 0));
        return;
      }
      const char* s = v ? "true" : "false";
      if (spec.ok && (spec.type == 's' || spec.type == '\0') && !spec.zero) {
        FmtSpec sspec = spec;
        sspec.prec = -1;  // precision on bool text: not meaningful, ignore
        RenderNumeric(out, cap, len, sspec, "s", s);
        return;
      }
      AppendRaw(out, cap, len, s, std::strlen(s));
      return;
    }
    case ArgType::kI64: {
      std::int64_t v;
      std::memcpy(&v, src, sizeof v);
      if (spec.ok && spec.prec < 0 && spec.type != '\0') {
        if (spec.type == 'd') {
          RenderNumeric(out, cap, len, spec, "lld",
                        static_cast<long long>(v));
          return;
        }
        if (spec.type == 'x' || spec.type == 'X' || spec.type == 'o') {
          const char conv[4] = {'l', 'l', spec.type, '\0'};
          RenderNumeric(out, cap, len, spec, conv,
                        static_cast<unsigned long long>(v));
          return;
        }
      } else if (spec.ok && spec.prec < 0 && spec.type == '\0') {
        RenderNumeric(out, cap, len, spec, "lld", static_cast<long long>(v));
        return;
      }
      FmtSpec none;
      RenderNumeric(out, cap, len, none, "lld", static_cast<long long>(v));
      return;
    }
    case ArgType::kU64: {
      std::uint64_t v;
      std::memcpy(&v, src, sizeof v);
      if (spec.ok && spec.prec < 0 &&
          (spec.type == '\0' || spec.type == 'd')) {
        RenderNumeric(out, cap, len, spec, "llu",
                      static_cast<unsigned long long>(v));
        return;
      }
      if (spec.ok && spec.prec < 0 &&
          (spec.type == 'x' || spec.type == 'X' || spec.type == 'o')) {
        const char conv[4] = {'l', 'l', spec.type, '\0'};
        RenderNumeric(out, cap, len, spec, conv,
                      static_cast<unsigned long long>(v));
        return;
      }
      FmtSpec none;
      RenderNumeric(out, cap, len, none, "llu",
                    static_cast<unsigned long long>(v));
      return;
    }
    case ArgType::kF64: {
      double v;
      std::memcpy(&v, src, sizeof v);
      char t = spec.ok ? spec.type : '\0';
      if (spec.ok &&
          (t == 'f' || t == 'F' || t == 'e' || t == 'E' || t == 'g' ||
           t == 'G' || t == '\0')) {
        const char conv[2] = {t == '\0' ? 'g' : t, '\0'};
        RenderNumeric(out, cap, len, spec, conv, v);
        return;
      }
      FmtSpec none;
      RenderNumeric(out, cap, len, none, "g", v);
      return;
    }
    case ArgType::kStr: {
      const char* s = reinterpret_cast<const char*>(src);
      std::size_t n = p.lens[i];
      if (spec.ok && (spec.type == 's' || spec.type == '\0') && !spec.zero) {
        if (spec.prec >= 0 && static_cast<std::size_t>(spec.prec) < n)
          n = static_cast<std::size_t>(spec.prec);  // fmt: prec truncates
        if (spec.width > static_cast<int>(n)) {
          // fmt default-aligns strings LEFT: text first, pad after.
          AppendRaw(out, cap, len, s, n);
          for (int k = static_cast<int>(n); k < spec.width && len < cap - 1;
               ++k)
            out[len++] = ' ';
          return;
        }
      }
      AppendRaw(out, cap, len, s, n);
      return;
    }
  }
}

// Format the deferred ArgPack against `fmt` into out (NUL-terminated,
// truncating). Each placeholder consumes the next argument; placeholders
// beyond the packed count render "{?}" (a dropped/overflowed argument).
void FormatMessage(char* out, std::size_t cap, const char* fmt,
                   const ArgPack& args) noexcept {
  std::size_t len = 0;
  std::uint8_t next = 0;
  for (const char* c = fmt; *c != '\0' && len < cap - 1; ++c) {
    if (c[0] == '{' && c[1] == '{') {
      out[len++] = '{';
      ++c;
    } else if (c[0] == '}' && c[1] == '}') {
      out[len++] = '}';
      ++c;
    } else if (*c == '{') {
      const char* body = c + 1;
      while (*c != '\0' && *c != '}') ++c;
      if (*c == '\0') break;  // unterminated placeholder: drop the tail
      FmtSpec spec;
      if (body == c) {
        spec.ok = true;  // "{}": default rendering
      } else if (*body == ':') {
        spec = ParseSpec(body + 1, c);  // ok=false => default fallback
      }  // positional/named ("{0}", "{name}"): ok=false => default fallback
      if (next < args.count) {
        RenderArg(out, cap, len, args, next++, spec);
      } else {
        AppendRaw(out, cap, len, "{?}", 3);
      }
    } else {
      out[len++] = *c;
    }
  }
  out[len] = '\0';
}

// ---- line assembly + atomic write ----------------------------------------------

const char* SeverityTag(Severity s) noexcept {
  switch (s) {
    case Severity::kTrace: return "trace";
    case Severity::kDebug: return "debug";
    case Severity::kInfo: return "info";
    case Severity::kWarn: return "warning";
    case Severity::kError: return "error";
    case Severity::kFatal: return "critical";
    case Severity::kOff: break;  // filter value, never emitted
  }
  return "?";
}

// ANSI whole-line color per severity (mirrors the previous colorized console
// output); empty when stderr is not a terminal.
bool StderrIsTty() noexcept {
#if defined(_WIN32) || defined(_WIN64)
  static const bool tty = _isatty(_fileno(stderr)) != 0;
#else
  static const bool tty = ::isatty(STDERR_FILENO) != 0;
#endif
  return tty;
}

const char* ColorFor(Severity s) noexcept {
  if (!StderrIsTty()) return "";
  switch (s) {
    case Severity::kTrace: return "\033[37m";
    case Severity::kDebug: return "\033[36m";
    case Severity::kInfo: return "\033[32m";
    case Severity::kWarn: return "\033[33m";
    case Severity::kError: return "\033[31m";
    case Severity::kFatal: return "\033[1;31m";
    case Severity::kOff: break;
  }
  return "";
}

const char* ColorReset() noexcept { return StderrIsTty() ? "\033[0m" : ""; }

// Short process name, resolved once (no allocation afterwards).
const char* ProcessName() noexcept {
  static char name[64] = {};
  static std::once_flag once;
  std::call_once(once, [] {
#if defined(__linux__)
    char path[1024];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) {
      path[n] = '\0';
      const char* base = std::strrchr(path, '/');
      std::snprintf(name, sizeof name, "%s", base != nullptr ? base + 1
                                                             : path);
      return;
    }
#endif
    std::snprintf(name, sizeof name, "%s", "xmbase");
  });
  return name;
}

// One complete line per write(2): concurrent emitters may order arbitrarily
// but never interleave mid-line (single syscall; a line is far below
// PIPE_BUF, so pipe/terminal writes are atomic).
void WriteLine(const char* data, std::size_t len) noexcept {
#if defined(_WIN32) || defined(_WIN64)
  std::fwrite(data, 1, len, stderr);
  std::fflush(stderr);
#else
  std::size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(STDERR_FILENO, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;  // stderr gone: nothing sensible left to do
    }
    off += static_cast<std::size_t>(n);
  }
#endif
}

// Assemble the standard prefix + message into one buffer and write it.
// Console timestamps are wall-clock (epoch seconds.nanoseconds), matching the
// previous output; the monotonic telemetry Timestamp is the SDK's currency.
void EmitLine(std::uint32_t source_id, Severity sev, const char* msg) noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const long long secs = static_cast<long long>(ns / 1000000000);
  const long long frac = static_cast<long long>(ns % 1000000000);

  const SourceEntry src = LookupSource(source_id);

  char line[768];
  int n;
  if (src.data != nullptr && src.len != 0) {
    n = std::snprintf(line, sizeof line,
                      "%s[%s] [%lld.%09lld] [%s]: [%.*s] %s%s\n",
                      ColorFor(sev), SeverityTag(sev), secs, frac,
                      ProcessName(), static_cast<int>(src.len), src.data, msg,
                      ColorReset());
  } else {
    n = std::snprintf(line, sizeof line, "%s[%s] [%lld.%09lld] [%s]: %s%s\n",
                      ColorFor(sev), SeverityTag(sev), secs, frac,
                      ProcessName(), msg, ColorReset());
  }
  if (n < 0) return;
  std::size_t len = static_cast<std::size_t>(n);
  if (len >= sizeof line) {  // truncated: keep it a complete line
    len = sizeof line - 1;
    line[len - 1] = '\n';
  }
  WriteLine(line, len);
}

// ---- binding entry points -------------------------------------------------------

void EmitEventImpl(std::uint32_t source_id, Severity sev, const char* fmt_str,
                   const ArgPack& args, Context, Timestamp) noexcept {
  if (!ShouldLogImpl(sev)) return;
  char msg[512];
  FormatMessage(msg, sizeof msg, fmt_str != nullptr ? fmt_str : "", args);
  EmitLine(source_id, sev, msg);
}

void EmitEventDynImpl(std::uint32_t source_id, Severity sev, const char* msg,
                      std::size_t len, Context, Timestamp) noexcept {
  if (!ShouldLogImpl(sev) || msg == nullptr) return;
  char buf[512];
  if (len >= sizeof buf) len = sizeof buf - 1;  // truncate gracefully
  std::memcpy(buf, msg, len);
  buf[len] = '\0';
  EmitLine(source_id, sev, buf);
}

void EmitSpanImpl(const char*, Context, SpanId, Timestamp, Timestamp,
                  const Context*, std::uint8_t) noexcept {
  // The recording plane arrives with the SDK; spans are dropped here.
}

void EmitSignalImpl(SignalSlot*, const void*, std::size_t,
                    Timestamp) noexcept {
  // Ditto — signals need a drain to mean anything.
}

const char* HealthName(HealthState s) noexcept {
  switch (s) {
    case HealthState::kOk: return "OK";
    case HealthState::kDegraded: return "DEGRADED";
    case HealthState::kFault: return "FAULT";
    case HealthState::kDisconnected: return "DISCONNECTED";
  }
  return "?";
}

void ReportHealthImpl(const char* subsystem, HealthState state,
                      const char* detail_msg, Context, Timestamp) noexcept {
  // State transitions are operator-relevant: one line, severity-mapped.
  const Severity sev = state == HealthState::kOk         ? Severity::kInfo
                       : state == HealthState::kDegraded ? Severity::kWarn
                                                         : Severity::kError;
  if (!ShouldLogImpl(sev)) return;
  char msg[512];
  const bool with_detail = detail_msg != nullptr && detail_msg[0] != '\0';
  std::snprintf(msg, sizeof msg, "[health] %s -> %s%s%s",
                subsystem != nullptr ? subsystem : "?", HealthName(state),
                with_detail ? ": " : "", with_detail ? detail_msg : "");
  EmitLine(0, sev, msg);
}

void SetResourceImpl(std::string_view, std::string_view) {
  // Resource attributes only mean something to an exporter; ignored here.
}

}  // namespace

const Binding* DefaultConsoleBinding() noexcept {
  static const Binding binding = [] {
    SetLevelImpl(LevelFromEnv(Severity::kInfo));  // seed once, then atomic
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

#else  // !ENABLE_LOGGING — no default binding; the true unbound state applies.

namespace xmotion {
namespace telemetry {
namespace detail {
const Binding* DefaultConsoleBinding() noexcept { return nullptr; }
}  // namespace detail
}  // namespace telemetry
}  // namespace xmotion

#endif  // ENABLE_LOGGING
