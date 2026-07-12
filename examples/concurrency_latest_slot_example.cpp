/*
 * concurrency_latest_slot_example.cpp
 *
 * The latest-value handoff: a sensor thread produces state at its own rate;
 * a consumer (control loop, UI, logger) always wants the NEWEST value and
 * never wants to wait for it.
 *
 * This is the pattern the deprecated container/ring_buffer was often bent
 * into ("push samples, drain the queue, keep the last one") — paying queue
 * management, memory, and latency for values that were thrown away. A
 * LatestSlot IS that pattern, directly: depth-1, wait-free writer, readers
 * retry only while a write is in flight, torn reads impossible.
 *
 * Contract highlights (see latest_slot.hpp for the memory-ordering proof):
 *   - Store() never blocks and never fails: new value overwrites unread old.
 *   - Load() returns false only if nothing was ever stored.
 *   - T must be trivially copyable (the seqlock copies raw words).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cstdio>
#include <thread>

#include "xmbase/concurrency/latest_slot.hpp"

namespace {

// Trivially-copyable state — the seqlock's requirement. A checksum field
// stands in for "the consumer can prove it never saw a torn value".
struct ImuState {
  std::uint64_t seq;
  double gyro[3];
  double accel[3];
  std::uint64_t checksum;

  static ImuState Make(std::uint64_t n) {
    ImuState s{n, {0.1 * n, 0.2 * n, 0.3 * n}, {1.0, 2.0, 3.0}, 0};
    s.checksum = s.seq * 7919u;
    return s;
  }
  bool Torn() const { return checksum != seq * 7919u; }
};

}  // namespace

int main() {
  xmotion::concurrency::LatestSlot<ImuState> slot;
  std::atomic<bool> running{true};

  // Producer: a "sensor" storing at full speed. Store() is wait-free —
  // this thread's timing can never be perturbed by a slow consumer.
  std::thread sensor([&] {
    std::uint64_t n = 0;
    while (running.load(std::memory_order_relaxed)) {
      slot.Store(ImuState::Make(++n));
    }
    std::printf("sensor: produced %lu states\n",
                static_cast<unsigned long>(n));
  });

  // Consumer: a slower "control loop" that only ever wants the newest
  // state. No draining, no backlog, no allocation — one Load per cycle.
  std::uint64_t cycles = 0, last_seq = 0, torn = 0;
  ImuState s{};
  while (cycles < 200000) {
    if (slot.Load(s)) {
      if (s.Torn()) ++torn;          // never happens: the seqlock forbids it
      last_seq = s.seq;
    }
    ++cycles;
  }
  running.store(false);
  sensor.join();

  std::printf(
      "consumer: %lu cycles, newest seq seen %lu, torn reads %lu\n"
      "(gaps between seen seqs are OVERWRITTEN-UNREAD values — correct "
      "latest-only behavior, not loss of the newest)\n",
      static_cast<unsigned long>(cycles), static_cast<unsigned long>(last_seq),
      static_cast<unsigned long>(torn));
  return torn == 0 ? 0 : 1;
}
