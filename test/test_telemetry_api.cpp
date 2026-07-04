/*
 * test_telemetry_api.cpp
 *
 * API-tier tests for xmbase/telemetry: the unbound contract (stderr Warn+,
 * safe no-ops), context/scope semantics, envelope round-trip, and the
 * binding seam exercised end-to-end with a fake in-test binding — an early
 * validation of the exact interface the xmTelemetry SDK will implement.
 * The full behavioral suite lives in the xmTelemetry scenario tests.
 */

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;

namespace {

// NOTE: xmBase auto-adopts the interim spdlog logging binding on first use
// (ENABLE_LOGGING builds). Tests of the TRUE unbound contract pin it with an
// explicit InstallBinding(nullptr) — an explicit unbind is authoritative and
// disables auto-adoption (see binding.hpp).

// ---------- unbound contract (S6 shape) --------------------------------------

TEST(TelemetryUnbound, WarnAndAboveReachStderrFormatted) {
  ASSERT_TRUE(tel::InstallBinding(nullptr));
  ASSERT_EQ(tel::ActiveBinding(), nullptr);
  testing::internal::CaptureStderr();
  XM_WARN("saturated cmd={} limit={}", 1.5, 2);
  XM_ERROR("bad state: {}", "estop");
  XM_INFO("must not appear {}", 42);
  XM_DEBUG("must not appear either");
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("[xmtelemetry:WARN] saturated cmd=1.5 limit=2"),
            std::string::npos)
      << err;
  EXPECT_NE(err.find("[xmtelemetry:ERROR] bad state: estop"),
            std::string::npos);
  EXPECT_EQ(err.find("must not appear"), std::string::npos);
}

TEST(TelemetryUnbound, HealthNonOkReachesStderr) {
  tel::InstallBinding(nullptr);
  testing::internal::CaptureStderr();
  tel::ReportHealth("imu", tel::HealthState::kOk, "fine");        // silent
  tel::ReportHealth("imu", tel::HealthState::kDegraded, "stale"); // reported
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("imu -> DEGRADED: stale"), std::string::npos) << err;
  EXPECT_EQ(err.find("OK"), std::string::npos);
}

struct Pod {
  double a;
  int b;
};

TEST(TelemetryUnbound, VerbsAreSafeNoops) {
  tel::InstallBinding(nullptr);
  auto counter = tel::GetCounter("t.counter");
  auto gauge = tel::GetGauge("t.gauge");
  auto histo = tel::GetHistogram("t.histo");
  auto ch = tel::GetChannel<Pod>("t.channel");
  counter.Add();
  counter.Add(2.5);
  gauge.Set(1.0);
  histo.Record(3.0);
  ch.Publish(Pod{1.0, 2});
  {
    XM_SCOPE("t.scope");
    { XM_SCOPE("t.nested"); }
  }
  SUCCEED();  // no crash, no output required
}

// ---------- context spine -----------------------------------------------------

TEST(TelemetryContext, NewTraceMintsValidDistinctIds) {
  const tel::Context a = tel::NewTrace();
  const tel::Context b = tel::NewTrace();
  EXPECT_TRUE(a.valid());
  EXPECT_TRUE(b.valid());
  EXPECT_FALSE(a.trace == b.trace);
  EXPECT_FALSE(a.span == b.span);
}

TEST(TelemetryContext, GuardSetsAndRestores) {
  EXPECT_FALSE(tel::CurrentContext().valid());  // fresh test thread default
  const tel::Context root = tel::NewTrace();
  {
    tel::ContextGuard g(root);
    EXPECT_TRUE(tel::CurrentContext().trace == root.trace);
    {
      tel::ContextGuard inner(tel::NewTrace());
      EXPECT_FALSE(tel::CurrentContext().trace == root.trace);
    }
    EXPECT_TRUE(tel::CurrentContext().trace == root.trace);
  }
  EXPECT_FALSE(tel::CurrentContext().valid());
}

TEST(TelemetryContext, InjectExtractRoundTrip) {
  const tel::Context ctx = tel::NewTrace();
  const auto wire = tel::Inject(ctx);
  const tel::Context back = tel::Extract(wire.data(), wire.size());
  EXPECT_TRUE(back.trace == ctx.trace);
  EXPECT_TRUE(back.span == ctx.span);
  // boundary validation: short / null input yields an invalid context
  EXPECT_FALSE(tel::Extract(wire.data(), 8).valid());
  EXPECT_FALSE(tel::Extract(nullptr, 24).valid());
}

TEST(TelemetryContext, IsThreadLocal) {
  const tel::Context root = tel::NewTrace();
  tel::ContextGuard g(root);
  tel::Context seen_on_worker = root;  // sentinel: must be overwritten
  std::thread([&] { seen_on_worker = tel::CurrentContext(); }).join();
  EXPECT_FALSE(seen_on_worker.valid());  // worker starts with no context
}

TEST(TelemetryScope, MaintainsNestingEvenUnbound) {
  const tel::Context root = tel::NewTrace();
  tel::ContextGuard g(root);
  const tel::SpanId root_span = tel::CurrentContext().span;
  {
    tel::Scope outer("outer");
    const tel::Context in_outer = tel::CurrentContext();
    EXPECT_TRUE(in_outer.trace == root.trace);       // same trace
    EXPECT_FALSE(in_outer.span == root_span);        // new span
    {
      tel::Scope inner("inner");
      EXPECT_FALSE(tel::CurrentContext().span == in_outer.span);
    }
    EXPECT_TRUE(tel::CurrentContext().span == in_outer.span);  // restored
  }
  EXPECT_TRUE(tel::CurrentContext().span == root_span);
}

// ---------- the binding seam, exercised with a fake SDK -----------------------

struct FakeSdk {
  static tel::detail::CounterSlot counter_slot;
  static tel::detail::GaugeSlot gauge_slot;
  static tel::detail::HistogramSlot histo_slot;
  static tel::detail::SignalSlot signal_slot;
  static std::vector<std::string> events;   // "sev|fmt"
  static std::vector<std::string> spans;    // name
  static std::vector<std::size_t> signals;  // payload sizes
  static std::vector<std::string> health;   // "subsystem|state"

  static tel::Binding Make() {
    tel::Binding b{};
    b.abi_version = tel::kBindingAbiVersion;
    b.get_counter = [](std::string_view) { return &counter_slot; };
    b.get_gauge = [](std::string_view) { return &gauge_slot; };
    b.get_histogram = [](std::string_view) { return &histo_slot; };
    b.get_signal = [](std::string_view, std::size_t, std::size_t,
                      const char*) { return &signal_slot; };
    b.intern_source = [](std::string_view) { return 7u; };
    b.should_log = [](tel::Severity) noexcept { return true; };
    b.emit_event = [](std::uint32_t, tel::Severity sev, const char* fmt,
                      const tel::detail::ArgPack&, tel::Context,
                      tel::Timestamp) noexcept {
      events.push_back(std::to_string(static_cast<int>(sev)) + "|" + fmt);
    };
    b.emit_event_dyn = [](std::uint32_t, tel::Severity sev, const char* msg,
                          std::size_t len, tel::Context,
                          tel::Timestamp) noexcept {
      events.push_back("dyn|" + std::to_string(static_cast<int>(sev)) + "|" +
                       std::string(msg, len));
    };
    b.emit_span = [](const char* name, tel::Context, tel::SpanId,
                     tel::Timestamp, tel::Timestamp) noexcept {
      spans.push_back(name);
    };
    b.emit_signal = [](tel::detail::SignalSlot*, const void*,
                       std::size_t size, tel::Timestamp) noexcept {
      signals.push_back(size);
    };
    b.report_health = [](const char* subsystem, tel::HealthState s,
                         const char*, tel::Context, tel::Timestamp) noexcept {
      health.push_back(std::string(subsystem) + "|" +
                       std::to_string(static_cast<int>(s)));
    };
    b.set_resource = [](std::string_view, std::string_view) {};
    return b;
  }
};
tel::detail::CounterSlot FakeSdk::counter_slot;
tel::detail::GaugeSlot FakeSdk::gauge_slot;
tel::detail::HistogramSlot FakeSdk::histo_slot;
tel::detail::SignalSlot FakeSdk::signal_slot;
std::vector<std::string> FakeSdk::events;
std::vector<std::string> FakeSdk::spans;
std::vector<std::size_t> FakeSdk::signals;
std::vector<std::string> FakeSdk::health;

class TelemetryBoundSeam : public ::testing::Test {
 protected:
  void SetUp() override {
    binding_ = FakeSdk::Make();
    ASSERT_TRUE(tel::InstallBinding(&binding_));
    FakeSdk::events.clear();
    FakeSdk::spans.clear();
    FakeSdk::signals.clear();
    FakeSdk::health.clear();
  }
  void TearDown() override { tel::InstallBinding(nullptr); }
  tel::Binding binding_;
};

TEST_F(TelemetryBoundSeam, AllVerbsRouteThroughTheBinding) {
  testing::internal::CaptureStderr();

  auto counter = tel::GetCounter("b.counter");
  counter.Add(2.0);
  counter.Add(3.0);
  EXPECT_DOUBLE_EQ(
      FakeSdk::counter_slot.value.load(std::memory_order_relaxed), 5.0);

  auto histo = tel::GetHistogram("b.histo");
  histo.Record(1.0);
  histo.Record(9.0);
  EXPECT_EQ(FakeSdk::histo_slot.count.load(), 2u);
  EXPECT_DOUBLE_EQ(FakeSdk::histo_slot.min.load(), 1.0);
  EXPECT_DOUBLE_EQ(FakeSdk::histo_slot.max.load(), 9.0);

  XM_INFO("bound info {}", 1);  // below Warn but BOUND -> routed, not dropped
  XM_WARN("bound warn");
  ASSERT_EQ(FakeSdk::events.size(), 2u);
  EXPECT_EQ(FakeSdk::events[0], "2|bound info {}");

  { XM_SCOPE("b.scope"); }
  ASSERT_EQ(FakeSdk::spans.size(), 1u);
  EXPECT_EQ(FakeSdk::spans[0], "b.scope");

  auto ch = tel::GetChannel<Pod>("b.channel");
  ch.Publish(Pod{1, 2});
  ASSERT_EQ(FakeSdk::signals.size(), 1u);
  EXPECT_EQ(FakeSdk::signals[0], sizeof(Pod));

  tel::ReportHealth("b.imu", tel::HealthState::kOk);  // Ok IS routed bound
  ASSERT_EQ(FakeSdk::health.size(), 1u);
  EXPECT_EQ(FakeSdk::health[0], "b.imu|0");

  // and none of it leaked to stderr
  EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
}

// THE unification proof: fmt-style and stream-style logging are ONE API —
// both flow through the identical binding seam as every other verb.
TEST_F(TelemetryBoundSeam, FmtAndStreamMacrosRouteThroughTheSameSpine) {
  XM_WARN("unified {}", 1);
  XM_INFO_STREAM("streamed " << 42);
  ASSERT_EQ(FakeSdk::events.size(), 2u);
  EXPECT_EQ(FakeSdk::events[0], "3|unified {}");
  EXPECT_EQ(FakeSdk::events[1], "dyn|2|streamed 42");
}

TEST_F(TelemetryBoundSeam, SourceAttributedEventsCarryInternedId) {
  const tel::EventSource src = tel::GetEventSource("b.motor");
  EXPECT_EQ(src.id(), 7u);
  XM_ERROR_SRC(src, "fault {}", 3);
  ASSERT_EQ(FakeSdk::events.size(), 1u);
  EXPECT_EQ(FakeSdk::events[0], "4|fault {}");
}

TEST(TelemetryBinding, AbiMismatchIsRejected) {
  tel::InstallBinding(nullptr);  // pin a known state (disables auto-adoption)
  tel::Binding bad = FakeSdk::Make();
  bad.abi_version = 999;
  EXPECT_FALSE(tel::InstallBinding(&bad));
  EXPECT_EQ(tel::ActiveBinding(), nullptr);
}

TEST(TelemetryBinding, UninstallRevertsToUnbound) {
  tel::Binding b = FakeSdk::Make();
  ASSERT_TRUE(tel::InstallBinding(&b));
  ASSERT_TRUE(tel::InstallBinding(nullptr));
  testing::internal::CaptureStderr();
  XM_WARN("back to stderr");
  EXPECT_NE(testing::internal::GetCapturedStderr().find("back to stderr"),
            std::string::npos);
}

// Handles acquired unbound stay inert after a binding installs (documented
// D9/D14 contract) — and remain SAFE, which is the part that matters.
TEST(TelemetryBinding, PreBindHandlesStayInertButSafe) {
  tel::InstallBinding(nullptr);
  auto early = tel::GetCounter("later");
  tel::Binding b = FakeSdk::Make();
  ASSERT_TRUE(tel::InstallBinding(&b));
  const double before = FakeSdk::counter_slot.value.load();
  early.Add(5.0);  // goes to the no-op slot, not the SDK slot
  EXPECT_DOUBLE_EQ(FakeSdk::counter_slot.value.load(), before);
  tel::InstallBinding(nullptr);
}

// ---------- deferred-format edge cases ----------------------------------------

TEST(TelemetryUnbound, FormatterHandlesEdgeCases) {
  tel::InstallBinding(nullptr);
  testing::internal::CaptureStderr();
  XM_WARN("brace {{literal}} kept, spec {:.3f} ignored-but-consumed", 1.5);
  XM_WARN("missing arg: {} {}", 1);  // second placeholder has no arg
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("brace {literal} kept, spec 1.5 ignored-but-consumed"),
            std::string::npos)
      << err;
  EXPECT_NE(err.find("missing arg: 1 {?}"), std::string::npos) << err;
}

}  // namespace
