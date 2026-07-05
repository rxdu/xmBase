/*
 * instrumented_control_loop_example.cpp
 *
 * REFERENCE USAGE — the canonical hot-loop instrumentation pattern (all four
 * verbs + health) from docs/telemetry/guide.md.
 *
 * Pattern: register handles at init; only atomics / ring pushes inside the
 * loop. With no SDK bound (this binary), events go through the built-in console
 * binding; metrics aggregate in-slot; scope/signal records are dropped until
 * an application binds the xmTelemetry SDK — the CALL SITES never change.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <chrono>
#include <cmath>
#include <thread>

#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;

namespace {

// High-rate signal payload: trivially copyable POD, fixed size.
struct ControlState {
  double setpoint;
  double measured;
  double command;
};

class Controller {
 public:
  // Handles acquired ONCE at init — never lookup-by-name on the hot path.
  Controller()
      : src_(tel::GetEventSource("demo.motor")),
        jitter_us_(tel::GetHistogram("demo.ctrl.jitter_us")),
        deadline_miss_(tel::GetCounter("demo.ctrl.deadline_miss")),
        temp_c_(tel::GetGauge("demo.motor.temp_c")),
        state_ch_(tel::GetChannel<ControlState>("demo.ctrl.state")) {}

  // The hot path: noexcept, allocation-free — atomics and bounded copies only.
  void Step(double setpoint, tel::Timestamp deadline) {
    XM_SPAN("demo.ctrl.cycle");  // span: cycle timing, auto-nested

    const double measured = plant_ += 0.2 * (command_ - plant_);
    command_ = 2.0 * (setpoint - measured);
    if (command_ > 1.0) {
      command_ = 1.0;
      XM_WARN_SRC(src_, "saturated: cmd clamped to {} (sp={:.2f})", 1.0, setpoint);
    }

    state_ch_.Publish(ControlState{setpoint, measured, command_});
    temp_c_.Set(35.0 + 0.01 * std::abs(command_));

    const auto now = tel::Now();
    if (now > deadline) deadline_miss_.Add();
    jitter_us_.Record(std::abs(
        std::chrono::duration<double, std::micro>(now - deadline).count()));
  }

  // Supervisor tick (non-RT): health TRANSITIONS with caller-side hysteresis.
  void Supervise(bool sensor_fresh) {
    const tel::HealthState next =
        sensor_fresh ? tel::HealthState::kOk : tel::HealthState::kDegraded;
    if (next != health_) {
      health_ = next;
      tel::ReportHealth("demo.motor", next,
                        sensor_fresh ? "recovered" : "sensor stale");
    }
  }

 private:
  tel::EventSource src_;
  tel::Histogram jitter_us_;
  tel::Counter deadline_miss_;
  tel::Gauge temp_c_;
  tel::SignalChannel<ControlState> state_ch_;
  double plant_ = 0.0, command_ = 0.0;
  tel::HealthState health_ = tel::HealthState::kOk;
};

}  // namespace

int main() {
  tel::SetResource("robot.id", "demo-bot");
  XM_INFO("instrumented control loop demo: 200 cycles at 100 Hz");

  Controller ctrl;
  const auto period = std::chrono::milliseconds(10);
  auto next = tel::Now() + period;
  for (int i = 0; i < 200; ++i) {
    ctrl.Step(/*setpoint=*/std::sin(0.05 * i), /*deadline=*/next);
    if (i == 60) ctrl.Supervise(false);   // simulate a stale sensor...
    if (i == 90) ctrl.Supervise(true);    // ...and recovery
    std::this_thread::sleep_until(next);
    next += period;
  }

  XM_INFO("done — with the xmTelemetry SDK bound, the same run would have "
          "recorded 200 spans + 200 signal samples and exported the metrics");
  return 0;
}
