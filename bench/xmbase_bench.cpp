/*
 * bench/xmbase_bench.cpp — xmBase pins its own primitive numbers.
 *
 * Publish/take-shaped micro benchmarks over the promoted concurrency
 * primitives (ADR 0007 W1), measured with xmbase/testing — the same
 * harness/probe every family suite uses, so the numbers stay comparable
 * with xmMessaging's M9 rows.
 *
 * The hot-path rows are ALLOC-GATED: a single allocation in a measured
 * section fails the run (exit code 1) — the R7 discipline enforced at the
 * primitive level.
 *
 * Usage: xmbase_bench [--out <report.json>] [--smoke] [--filter <substr>]
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "xmbase/concurrency/spsc_queue.hpp"
#include "xmbase/concurrency/message_buffer.hpp"
#include "xmbase/concurrency/event_count.hpp"
#include "xmbase/testing/alloc_probe.hpp"  // ONE translation unit per binary
#include "xmbase/testing/bench_harness.hpp"

namespace {

using xmotion::concurrency::SpscQueue;
using xmotion::concurrency::EventCount;
using xmotion::concurrency::HeapStorage;
using xmotion::concurrency::MessageBuffer;
using xmotion::concurrency::MutexMessageBuffer;
using xmotion::testing::AllocProbe;
using xmotion::testing::BenchResult;
using xmotion::testing::MeasureBatched;
using xmotion::testing::MeasureBatchedWithSetup;

template <std::size_t kBytes>
struct Payload {
  static_assert(kBytes % 8 == 0, "keep payloads word-shaped");
  std::uint64_t words[kBytes / 8] = {};
};

double g_scale = 1.0;
bool g_smoke = false;
std::string g_filter;
std::vector<BenchResult> g_results;
std::vector<std::string> g_gate_failures;

bool Enabled(const std::string& name) {
  return g_filter.empty() || name.find(g_filter) != std::string::npos;
}

int ScaledSamples(int samples, int floor_samples = 50) {
  const int scaled = static_cast<int>(samples * g_scale);
  return scaled < floor_samples ? floor_samples : scaled;
}

void Record(BenchResult result) {
  if (result.alloc_gated && result.allocations != 0) {
    g_gate_failures.push_back(result.name + ": " +
                              std::to_string(result.allocations) +
                              " allocation(s) in the measured section");
  }
  std::printf("%-40s p50 %9.1f  p99 %9.1f  p99.9 %9.1f  max %10.1f ns"
              "  (n=%zu%s, alloc=%llu%s)\n",
              result.name.c_str(), result.stats.p50, result.stats.p99,
              result.stats.p999, result.stats.max, result.stats.samples,
              result.batch > 1 ? ", batched" : "",
              static_cast<unsigned long long>(result.allocations),
              result.alloc_gated ? " gated" : "");
  g_results.push_back(std::move(result));
}

// Batch sizes: amortize the ~20 ns clock read below the op cost, small
// enough that batch means still expose tails (batch is recorded per row).
constexpr int kBatch = 256;

template <std::size_t kBytes>
void BenchLatestStore() {
  const std::string name =
      "micro/latest_store/" + std::to_string(kBytes) + "B";
  if (!Enabled(name)) {
    return;
  }
  MessageBuffer<Payload<kBytes>, HeapStorage, EventCount> slot;
  Payload<kBytes> value;
  value.words[0] = 1;

  BenchResult r;
  r.name = name;
  r.layer = "micro";
  r.payload_bytes = kBytes;
  r.batch = kBatch;
  r.alloc_gated = true;
  r.stats = MeasureBatched<AllocProbe>(
      [&] {
        ++value.words[0];
        slot.Store(value);
      },
      kBatch, ScaledSamples(400), &r.allocations);
  Record(std::move(r));
}

template <std::size_t kBytes>
void BenchLatestLoad() {
  const std::string name = "micro/latest_load/" + std::to_string(kBytes) + "B";
  if (!Enabled(name)) {
    return;
  }
  MessageBuffer<Payload<kBytes>, HeapStorage, EventCount> slot;
  Payload<kBytes> value;
  value.words[0] = 7;
  slot.Store(value);
  Payload<kBytes> out;

  BenchResult r;
  r.name = name;
  r.layer = "micro";
  r.payload_bytes = kBytes;
  r.batch = kBatch;
  r.alloc_gated = true;
  std::uint64_t sink = 0;
  r.stats = MeasureBatched<AllocProbe>(
      [&] {
        slot.Load(out);
        sink += out.words[0];
      },
      kBatch, ScaledSamples(400), &r.allocations);
  if (sink == 0) {
    r.notes = "sink zero (unexpected)";
  }
  Record(std::move(r));
}

template <std::size_t kBytes>
void BenchQueuePushPop() {
  const std::string name =
      "micro/queue_push_pop/" + std::to_string(kBytes) + "B";
  if (!Enabled(name)) {
    return;
  }
  SpscQueue<Payload<kBytes>> queue(16);
  Payload<kBytes> value;
  Payload<kBytes> out;

  BenchResult r;
  r.name = name;
  r.layer = "micro";
  r.payload_bytes = kBytes;
  r.batch = kBatch;
  r.alloc_gated = true;
  r.stats = MeasureBatched<AllocProbe>(
      [&] {
        ++value.words[0];
        queue.TryPush(value);
        queue.TryPop(out);
      },
      kBatch, ScaledSamples(400), &r.allocations);
  Record(std::move(r));
}

void BenchMutexLatestStoreLoad() {
  const std::string name = "micro/mutex_latest_store_load/64B";
  if (!Enabled(name)) {
    return;
  }
  MutexMessageBuffer<Payload<64>> slot;
  Payload<64> value;
  Payload<64> out;

  BenchResult r;
  r.name = name;
  r.layer = "micro";
  r.payload_bytes = 64;
  r.batch = kBatch;
  r.alloc_gated = true;  // the fallback takes a lock, but must not allocate
  r.stats = MeasureBatched<AllocProbe>(
      [&] {
        ++value.words[0];
        slot.Store(value);
        slot.Load(out);
      },
      kBatch, ScaledSamples(400), &r.allocations);
  Record(std::move(r));
}

void BenchWaiterNotifyUncontended() {
  const std::string name = "micro/futex_notify_all_uncontended";
  if (!Enabled(name)) {
    return;
  }
  EventCount waiter;

  BenchResult r;
  r.name = name;
  r.layer = "micro";
  r.payload_bytes = 0;
  r.batch = 64;  // a syscall per op: smaller batch keeps samples honest
  r.alloc_gated = true;
  r.stats = MeasureBatched<AllocProbe>([&] { waiter.NotifyAll(); }, r.batch,
                                       ScaledSamples(200), &r.allocations);
  Record(std::move(r));
}

// Store under continuous concurrent readers: the writer's wait-freedom
// claim, measured (readers never slow the writer beyond cache effects).
template <std::size_t kBytes>
void BenchLatestStoreContended() {
  const std::string name =
      "contended/latest_store_2readers/" + std::to_string(kBytes) + "B";
  if (!Enabled(name)) {
    return;
  }
  MessageBuffer<Payload<kBytes>, HeapStorage, EventCount> slot;
  Payload<kBytes> value;
  value.words[0] = 1;
  slot.Store(value);

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 2; ++i) {
    readers.emplace_back([&] {
      Payload<kBytes> out;
      while (!stop.load(std::memory_order_acquire)) {
        slot.Load(out);
      }
    });
  }

  BenchResult r;
  r.name = name;
  r.layer = "contended";
  r.payload_bytes = kBytes;
  r.contended = true;
  r.batch = kBatch;
  r.alloc_gated = true;
  r.stats = MeasureBatched<AllocProbe>(
      [&] {
        ++value.words[0];
        slot.Store(value);
      },
      kBatch, ScaledSamples(400), &r.allocations);

  stop.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }
  Record(std::move(r));
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_path = "xmbase_bench_report.json";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--smoke") {
      g_smoke = true;
      g_scale = 0.15;
    } else if (arg == "--filter" && i + 1 < argc) {
      g_filter = argv[++i];
    } else {
      std::fprintf(stderr,
                   "usage: %s [--out <report.json>] [--smoke] "
                   "[--filter <substr>]\n",
                   argv[0]);
      return 2;
    }
  }

  const auto hw = xmotion::testing::CaptureHardwareContext();
  std::printf("xmbase_bench — primitive numbers (%s)\n",
              g_smoke ? "smoke" : "full");
  std::printf("  cpu      : %s (nproc %u, governor %s)\n",
              hw.cpu_model.c_str(), hw.nproc, hw.governor.c_str());
  std::printf("  kernel   : %s%s\n\n", hw.kernel.c_str(),
              hw.preempt_rt ? " PREEMPT_RT" : "");

  BenchLatestStore<16>();
  BenchLatestStore<64>();
  BenchLatestStore<256>();
  BenchLatestLoad<64>();
  BenchQueuePushPop<64>();
  BenchQueuePushPop<256>();
  BenchMutexLatestStoreLoad();
  BenchWaiterNotifyUncontended();
  BenchLatestStoreContended<64>();

  if (!xmotion::testing::WriteJsonReport(out_path, "xmbase-bench-v1", hw,
                                         g_results, g_smoke, g_scale,
                                         g_gate_failures)) {
    std::fprintf(stderr, "xmbase_bench: failed to write %s\n",
                 out_path.c_str());
    return 1;
  }
  std::printf("\nreport: %s\n", out_path.c_str());

  if (!g_gate_failures.empty()) {
    std::fprintf(stderr, "\nALLOC GATE FAILED:\n");
    for (const auto& failure : g_gate_failures) {
      std::fprintf(stderr, "  %s\n", failure.c_str());
    }
    return 1;
  }
  return 0;
}
