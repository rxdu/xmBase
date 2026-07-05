/*
 * test_logging.cpp
 *
 * Description: Unit tests for the built-in console binding — the permanent,
 *              dependency-free default backend of the XM_* logging macros
 *              (auto-adopted on first use): default level, runtime level
 *              control, the spec-subset formatter, truncation contracts,
 *              dyn/stream pass-through, health lines, mid-line write
 *              atomicity under concurrency, and the explicit-unbind latch.
 *              Each TEST runs in its own process under ctest (via
 *              gtest_discover_tests), so adoption state and level changes do
 *              not leak between cases; tests still set the level explicitly
 *              so a whole-binary run (e.g. under sanitizers) passes too. The
 *              latch test disables auto-adoption for the remainder of its
 *              process, so it MUST stay last in this file.
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;
using tel::Severity;

namespace {

// Capture everything written to fd 2 (the console binding writes with raw
// write(2), which gtest's CaptureStderr also handles — but we keep our own
// helper so the pipe capacity and EOF semantics are explicit).
class StderrCapture {
 public:
  void Start() {
    fflush(stderr);
    saved_ = dup(STDERR_FILENO);
    ASSERT_NE(saved_, -1);
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    read_end_ = fds[0];
    ASSERT_NE(dup2(fds[1], STDERR_FILENO), -1);
    close(fds[1]);
  }

  std::string Stop() {
    fflush(stderr);
    dup2(saved_, STDERR_FILENO);  // closes the pipe's last write end
    close(saved_);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(read_end_, buf, sizeof buf)) > 0)
      out.append(buf, static_cast<std::size_t>(n));
    close(read_end_);
    return out;
  }

 private:
  int saved_ = -1;
  int read_end_ = -1;
};

// Convenience: capture stderr around a callable.
template <typename Fn>
std::string CaptureLines(Fn&& fn) {
  StderrCapture cap;
  cap.Start();
  fn();
  return cap.Stop();
}

// The message part of the emitted line (strips "[sev] [ts] [proc]: ").
// Returns the whole capture when the prefix shape is absent.
std::string MessageOf(const std::string& line) {
  const std::size_t colon = line.find("]: ");
  return colon == std::string::npos ? line : line.substr(colon + 3);
}

// Standard line shape: [sev] [secs.9-digit-nanos] [process]: message
const std::regex kLinePattern(
    R"(\[(trace|debug|info|warning|error|critical)\] \[[0-9]+\.[0-9]{9}\] \[[^\]]+\]: .*)");

// Streamable probe that records whether its operator<< actually ran — proves
// the _STREAM macros do NOT build the message when the level is disabled.
int g_probe_evals = 0;
struct EvalProbe {};
std::ostream& operator<<(std::ostream& os, const EvalProbe&) {
  ++g_probe_evals;
  return os << "probed";
}

}  // namespace

#ifndef ENABLE_LOGGING

// The console binding is compiled out: there is nothing to test except that
// no binding gets auto-adopted (the true unbound state applies).
TEST(ConsoleBindingTest, DisabledBuildAdoptsNoBinding) {
  EXPECT_EQ(tel::ActiveBinding(), nullptr);
}

#else  // ENABLE_LOGGING

// ---- level behavior -----------------------------------------------------------

TEST(ConsoleBindingTest, InfoVisibleByDefaultDebugSuppressed) {
  // Must run against the freshly adopted binding with no level override.
  unsetenv("XLOG_LEVEL");
  const std::string out = CaptureLines([] {
    XM_INFO("default level check {}", 1);
    XM_DEBUG("must not appear {}", 2);
  });
  EXPECT_NE(out.find("[info]"), std::string::npos) << out;
  EXPECT_NE(out.find("default level check 1"), std::string::npos) << out;
  EXPECT_EQ(out.find("must not appear"), std::string::npos) << out;
}

TEST(ConsoleBindingTest, SetLogLevelEnablesDebug) {
  tel::SetLogLevel(Severity::kInfo);
  std::string out = CaptureLines([] { XM_DEBUG("hidden {}", 1); });
  EXPECT_EQ(out.find("hidden"), std::string::npos) << out;

  tel::SetLogLevel(Severity::kDebug);
  out = CaptureLines([] { XM_DEBUG("revealed {}", 2); });
  EXPECT_NE(out.find("[debug]"), std::string::npos) << out;
  EXPECT_NE(out.find("revealed 2"), std::string::npos) << out;
  tel::SetLogLevel(Severity::kInfo);
}

TEST(ConsoleBindingTest, SetLevelIsReflectedByGetLevel) {
  tel::SetLogLevel(Severity::kInfo);
  EXPECT_EQ(tel::GetLogLevel(), Severity::kInfo);
  tel::SetLogLevel(Severity::kError);
  EXPECT_EQ(tel::GetLogLevel(), Severity::kError);
  tel::SetLogLevel(Severity::kTrace);
  EXPECT_EQ(tel::GetLogLevel(), Severity::kTrace);
  tel::SetLogLevel(Severity::kOff);  // filter value: silence everything
  EXPECT_EQ(tel::GetLogLevel(), Severity::kOff);
  const std::string out =
      CaptureLines([] { XM_FATAL("silenced by kOff {}", 1); });
  EXPECT_EQ(out.find("silenced"), std::string::npos) << out;
  tel::SetLogLevel(Severity::kInfo);
}

// ---- line shape ---------------------------------------------------------------

TEST(ConsoleBindingTest, LineShapeMatchesConsoleFormat) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out =
      CaptureLines([] { XM_WARN("shape check {}", 7); });
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(std::regex_match(out.substr(0, out.find('\n')), kLinePattern))
      << out;
  EXPECT_NE(out.find("[warning]"), std::string::npos) << out;
  EXPECT_EQ(MessageOf(out), "shape check 7\n");
}

TEST(ConsoleBindingTest, SourceAttributedEventsCarrySourceTag) {
  tel::SetLogLevel(Severity::kInfo);
  const tel::EventSource src = tel::GetEventSource("imu");
  const std::string out =
      CaptureLines([&] { XM_WARN_SRC(src, "bias reset {}", 3); });
  EXPECT_NE(out.find("]: [imu] bias reset 3"), std::string::npos) << out;
}

// ---- spec-subset formatter ------------------------------------------------------

TEST(ConsoleBindingTest, DefaultRenderingPerArgType) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    XM_INFO("i={} u={} f={} b={} s={} cs={}", -42, 42u, 2.5, true,
            std::string_view("view"), "cstr");
  });
  EXPECT_EQ(MessageOf(out), "i=-42 u=42 f=2.5 b=true s=view cs=cstr\n") << out;
}

TEST(ConsoleBindingTest, SupportedSpecsFormatViaSnprintf) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    XM_INFO("a={:08.3f} b={:x} c={:X} d={:o} e={:6d} f={:.2e} g={:.3}",
            3.14159, 255, 255, 8, 42, 12345.678, 3.14159);
  });
  EXPECT_EQ(MessageOf(out),
            "a=0003.142 b=ff c=FF d=10 e=    42 f=1.23e+04 g=3.14\n")
      << out;
}

TEST(ConsoleBindingTest, StringWidthAndPrecision) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    // fmt semantics: strings default-align LEFT; precision truncates.
    XM_INFO("[{:8s}] [{:.3s}] [{:.3}]", "ab", "abcdef", "abcdef");
  });
  EXPECT_EQ(MessageOf(out), "[ab      ] [abc] [abc]\n") << out;
}

TEST(ConsoleBindingTest, UnsupportedSpecsFallBackToDefaultRendering) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    // '>' alignment, '+' sign, positional index, type mismatch ('f' on int,
    // 'd' on double): all outside the subset — argument still renders with
    // its default formatting, nothing is dropped or garbled.
    XM_INFO("a={:>10} b={:+d} c={0} d={:f} e={:d}", 42, 43, 44, 45, 2.5);
  });
  EXPECT_EQ(MessageOf(out), "a=42 b=43 c=44 d=45 e=2.5\n") << out;
}

TEST(ConsoleBindingTest, EscapedBracesAndMissingArgs) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    XM_INFO("lit={{}} v={} missing={}", 5);  // 2 placeholders, 1 arg
  });
  EXPECT_EQ(MessageOf(out), "lit={} v=5 missing={?}\n") << out;
}

// ---- ArgPack / line truncation contracts ----------------------------------------

TEST(ConsoleBindingTest, ArgsBeyondPackCapacityRenderAsDropped) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    // ArgPack::kMaxArgs == 8: the 9th argument is dropped at pack time and
    // its placeholder renders "{?}".
    XM_INFO("{} {} {} {} {} {} {} {} {}", 1, 2, 3, 4, 5, 6, 7, 8, 9);
  });
  EXPECT_EQ(MessageOf(out), "1 2 3 4 5 6 7 8 {?}\n") << out;
}

TEST(ConsoleBindingTest, LongStringArgTruncatesToPackBuffer) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string long_arg(200, 'x');  // > ArgPack::kBufBytes (160)
  const std::string out =
      CaptureLines([&] { XM_INFO("{}", std::string_view(long_arg)); });
  const std::string msg = MessageOf(out);
  // Exactly the first 160 bytes survive the pack; nothing beyond.
  EXPECT_EQ(msg, std::string(tel::detail::ArgPack::kBufBytes, 'x') + "\n")
      << msg.size();
}

TEST(ConsoleBindingTest, OverlongLineTruncatesButStaysOneLine) {
  tel::SetLogLevel(Severity::kInfo);
  // A format string much longer than the 512 B message buffer: the line must
  // truncate gracefully — still exactly one complete newline-terminated line.
  static const std::string big(1500, 'y');
  const std::string out =
      CaptureLines([&] { XM_INFO_STREAM("head " << big << " tail"); });
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.back(), '\n');
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1) << out.size();
  EXPECT_NE(out.find("head yyy"), std::string::npos);
  EXPECT_EQ(out.find("tail"), std::string::npos);  // truncated away
}

// ---- dyn / stream path ----------------------------------------------------------

TEST(ConsoleBindingTest, StreamEventsPassThroughPreformatted) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines(
      [] { XM_INFO_STREAM("value=" << 42 << " state=" << "armed"); });
  EXPECT_EQ(MessageOf(out), "value=42 state=armed\n") << out;
}

TEST(ConsoleBindingTest, StreamMacroSkipsArgEvalWhenLevelDisabled) {
  g_probe_evals = 0;
  tel::SetLogLevel(Severity::kError);  // debug disabled
  XM_DEBUG_STREAM("v=" << EvalProbe{});
  EXPECT_EQ(g_probe_evals, 0) << "disabled stream-log must not evaluate args";

  tel::SetLogLevel(Severity::kTrace);  // debug enabled
  const std::string out =
      CaptureLines([] { XM_DEBUG_STREAM("v=" << EvalProbe{}); });
  EXPECT_EQ(g_probe_evals, 1) << "enabled stream-log must evaluate args once";
  EXPECT_NE(out.find("v=probed"), std::string::npos) << out;
  tel::SetLogLevel(Severity::kInfo);
}

// ---- health --------------------------------------------------------------------

TEST(ConsoleBindingTest, HealthTransitionLogsOneLine) {
  tel::SetLogLevel(Severity::kInfo);
  const std::string out = CaptureLines([] {
    tel::ReportHealth("motor_left", tel::HealthState::kDegraded, "overtemp");
  });
  EXPECT_NE(out.find("[warning]"), std::string::npos) << out;
  EXPECT_NE(out.find("[health] motor_left -> DEGRADED: overtemp"),
            std::string::npos)
      << out;
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1);
}

// ---- misc macro composition ------------------------------------------------------

TEST(ConsoleBindingTest, MacrosComposeInUnbracedIfElse) {
  // Compiles only because the macros are expression/do-while(0) wrapped —
  // the successful compile IS the assertion.
  int x = 1;
  if (x == 2)
    XM_INFO("two");
  else
    XM_INFO("not two");

  for (int i = 0; i < 2; ++i)
    if (i == 0)
      XM_TRACE("t");
    else
      XM_DEBUG("d");

  SUCCEED();
}

// ---- concurrency: no mid-line interleaving ---------------------------------------

TEST(ConsoleBindingTest, ConcurrentEmitYieldsWholeLines) {
  tel::SetLogLevel(Severity::kInfo);
  constexpr int kThreads = 4;
  constexpr int kLinesPerThread = 50;

  const std::string out = CaptureLines([] {
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([t] {
        for (int i = 0; i < kLinesPerThread; ++i)
          XM_INFO("thread {} line {} payload deadbeef", t, i);
      });
    }
    for (auto& w : workers) w.join();
  });

  const std::regex line_re(
      R"(\[info\] \[[0-9]+\.[0-9]{9}\] \[[^\]]+\]: thread [0-3] line [0-9]+ payload deadbeef)");
  std::istringstream lines(out);
  std::string line;
  int count = 0;
  while (std::getline(lines, line)) {
    EXPECT_TRUE(std::regex_match(line, line_re))
        << "torn/interleaved line: '" << line << "'";
    ++count;
  }
  EXPECT_EQ(count, kThreads * kLinesPerThread);
}

// ---- explicit unbind latch (MUST STAY LAST: disables auto-adoption for the
// remainder of this process) -------------------------------------------------------

TEST(ConsoleBindingTest, ExplicitInstallNullptrRestoresUnboundSilence) {
  // Force adoption first, then explicitly unbind: the latch makes the
  // explicit nullptr authoritative — the console binding must never be
  // auto-adopted again in this process.
  tel::SetLogLevel(Severity::kInfo);
  ASSERT_NE(tel::ActiveBinding(), nullptr);
  ASSERT_TRUE(tel::InstallBinding(nullptr));
  ASSERT_EQ(tel::ActiveBinding(), nullptr);

  const std::string out = CaptureLines([] {
    XM_INFO("gone quiet {}", 1);   // sub-Warn: dropped when unbound
    XM_WARN("unbound warn {}", 2); // Warn+: unbound stderr fallback
  });
  EXPECT_EQ(out.find("gone quiet"), std::string::npos) << out;
  EXPECT_EQ(out.find("[warning]"), std::string::npos)
      << "console binding must not re-adopt after explicit unbind: " << out;
  EXPECT_NE(out.find("[xmtelemetry:WARN] unbound warn 2"), std::string::npos)
      << out;
}

#endif  // ENABLE_LOGGING
