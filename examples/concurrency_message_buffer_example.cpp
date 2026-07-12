/*
 * concurrency_message_buffer_example.cpp
 *
 * The recent-history window: MessageBuffer<T, N> keeps the last N values;
 * readers take newest-first Snapshots WITHOUT consuming anything and the
 * producer is never blocked or slowed.
 *
 * The motivating robotics case is delayed-measurement alignment in sensor
 * fusion: an IMU stores at 1 kHz; a GPS fix arrives ~80 ms late; the filter
 * snapshots the IMU window, finds the samples straddling the fix timestamp,
 * and re-integrates from there. The producer (a driver thread) must not
 * even notice the reader exists.
 *
 * Contract highlights (proof in detail/seqlock_ring.hpp):
 *   - Store() is wait-free and FLAT IN N (~8 ns for 64 B at N=1/4/16).
 *   - Snapshot(out, k) is wait-free, allocation-free, newest-first; every
 *     entry is torn-free with strictly consecutive descending positions —
 *     a concurrent writer can only SHORTEN THE TAIL, never corrupt it.
 *   - Nothing is consumed: two readers can window the same history.
 *
 * Single-value case -> MessageSlot (concurrency_message_slot_example.cpp).
 * Consume-each-exactly-once -> SpscQueue (concurrency_worker_inbox_example).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cstdio>
#include <thread>

#include "xmbase/concurrency/message_buffer.hpp"

namespace {

struct ImuSample {
  std::uint64_t t_us;   // sample timestamp (monotone in this demo)
  double gyro_z;
  std::uint64_t check;  // torn-read detector

  static ImuSample Make(std::uint64_t t) {
    return {t, 0.001 * static_cast<double>(t), t * 2654435761u};
  }
  bool Torn() const { return check != t_us * 2654435761u; }
};

}  // namespace

int main() {
  // Wiring time: depth is declared, memory fixed from here on (R7).
  // 256 samples @ 1 kHz = a 256 ms window — enough for an 80 ms-late fix.
  xmotion::concurrency::MessageBuffer<ImuSample, 256> imu_history;
  std::atomic<bool> running{true};

  // Producer: the "IMU driver thread", storing at full speed. Store is
  // wait-free — snapshotting readers cannot perturb its timing.
  std::thread imu([&] {
    std::uint64_t t = 0;
    while (running.load(std::memory_order_relaxed)) {
      imu_history.Store(ImuSample::Make(++t));
    }
    std::printf("imu: stored %lu samples\n", static_cast<unsigned long>(t));
  });

  // Consumer: the "fusion thread". A late fix arrives; window the history
  // and locate the samples around the fix time — no allocation, nothing
  // consumed, producer untouched.
  std::uint64_t windows = 0, torn = 0, gaps = 0, shortened = 0;
  ImuSample window[64];
  while (windows < 100000) {
    const std::size_t n = imu_history.Snapshot(window, 64);
    if (n == 0) continue;  // nothing stored yet
    for (std::size_t i = 0; i < n; ++i) {
      if (window[i].Torn()) ++torn;                       // never happens
      if (i > 0 && window[i - 1].t_us != window[i].t_us + 1) ++gaps;  // never
    }
    if (n < 64) ++shortened;  // startup or a writer lap trimmed the tail
    ++windows;
    // ... a real filter would now find window[j].t_us <= fix_time and
    // re-integrate from there ...
  }
  running.store(false);
  imu.join();

  std::printf(
      "fusion: %lu windows, torn %lu, ordering gaps %lu (both must be 0), "
      "tail-shortened windows %lu (laps/startup — expected, counted)\n",
      static_cast<unsigned long>(windows), static_cast<unsigned long>(torn),
      static_cast<unsigned long>(gaps), static_cast<unsigned long>(shortened));
  return (torn == 0 && gaps == 0) ? 0 : 1;
}
