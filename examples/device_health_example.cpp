/*
 * device_health_example.cpp
 *
 * REFERENCE USAGE — the device-driver telemetry pattern (mirrors scenario S3;
 * this is exactly how xmDriver's drivers adopt the API): an EventSource per
 * device, a freshness-age gauge, health TRANSITIONS with hysteresis (never
 * flapping), and attributed fault events. A monitoring side observes the
 * device purely from telemetry — it never touches the driver object.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <chrono>
#include <thread>

#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;
using namespace std::chrono_literals;

namespace {

class FakeImuDriver {
 public:
  FakeImuDriver()
      : src_(tel::GetEventSource("imu.front")),          // one tag per device
        age_ms_(tel::GetGauge("imu.front.age_ms")),
        faults_(tel::GetCounter("imu.front.faults")) {}

  // Called on every good sample (the FreshnessMonitor idiom of xmDriver).
  void OnSample() {
    last_ns_.store(tel::Now().time_since_epoch().count(),
                   std::memory_order_relaxed);
  }

  // Supervisor tick (non-RT): publish age; report health TRANSITIONS with
  // hysteresis — a state must hold for kHoldTicks before it is reported, so
  // an age oscillating around the limit never flaps Ok<->Degraded (S3-A5).
  void Supervise() {
    const auto age = tel::Now().time_since_epoch() -
                     tel::Duration(last_ns_.load(std::memory_order_relaxed));
    const double age_ms = std::chrono::duration<double, std::milli>(age).count();
    age_ms_.Set(age_ms);

    tel::HealthState raw = tel::HealthState::kOk;
    if (age > 200ms) raw = tel::HealthState::kDisconnected;
    else if (age > 50ms) raw = tel::HealthState::kDegraded;

    if (raw == candidate_) {
      if (++held_ >= kHoldTicks && raw != reported_) {
        reported_ = raw;
        tel::ReportHealth("imu.front", raw,
                          raw == tel::HealthState::kOk ? "recovered"
                                                       : "stale sample stream");
        if (raw == tel::HealthState::kDisconnected) {
          faults_.Add();
          XM_ERROR_SRC(src_, "no samples for {:.0f} ms — treating as disconnected",
                       age_ms);
        }
      }
    } else {
      candidate_ = raw;   // state changed: restart the hysteresis window
      held_ = 0;
    }
  }

 private:
  static constexpr int kHoldTicks = 3;
  tel::EventSource src_;
  tel::Gauge age_ms_;
  tel::Counter faults_;
  std::atomic<long long> last_ns_{0};
  tel::HealthState reported_ = tel::HealthState::kOk;
  tel::HealthState candidate_ = tel::HealthState::kOk;
  int held_ = 0;
};

}  // namespace

int main() {
  XM_INFO("device health demo: healthy -> cable pull -> recovery");
  FakeImuDriver imu;

  auto run = [&](std::chrono::milliseconds dur, bool feeding) {
    const auto until = tel::Now() + dur;
    while (tel::Now() < until) {
      if (feeding) imu.OnSample();
      imu.Supervise();
      std::this_thread::sleep_for(10ms);
    }
  };

  run(200ms, true);    // healthy
  run(600ms, false);   // "cable pull": Degraded then Disconnected (hysteresis-held)
  imu.OnSample();
  run(200ms, true);    // recovery -> Ok

  XM_INFO("done — transitions were reported once each, no flapping");
  return 0;
}
