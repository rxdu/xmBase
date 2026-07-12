/*
 * concurrency_region_storage_example.cpp
 *
 * The Storage seam: the SAME MessageBuffer algorithm, placed in memory the
 * CALLER owns — here, a MAP_SHARED mapping crossing a fork(), so a child
 * process stores IMU samples and the parent snapshots them, with no
 * messaging layer, no serialization, no copies beyond the seqlock reads.
 *
 * This is the placement bet made reusable: HeapStorage (the default) and
 * RegionStorage run the identical, identically-verified algorithm — the
 * region variant is how xmMessaging hosts these rings in named shared-
 * memory segments, and how any application can do the same ad hoc.
 *
 * The three RegionStorage rules on display:
 *   1. Size with StorageBytes() — the layout is a pure function of (T, N),
 *      never of who attached first.
 *   2. Exactly ONE side constructs with initialize=true (zeroes the cells);
 *      every other side attaches with initialize=false and sees whatever
 *      the region already holds (the warm-start / crash-survivor story).
 *   3. Region-placed payloads must be trivially copyable/destructible —
 *      the region owns the bytes; nothing is ever destroyed.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>

#include "xmbase/concurrency/message_buffer.hpp"
#include "xmbase/concurrency/storage.hpp"

namespace {

struct ImuSample {
  std::uint64_t t_us;
  double gyro_z;
  std::uint64_t check;

  static ImuSample Make(std::uint64_t t) {
    return {t, 0.001 * static_cast<double>(t), t * 2654435761u};
  }
  bool Torn() const { return check != t_us * 2654435761u; }
};

using xmotion::concurrency::MessageBuffer;
using xmotion::concurrency::RegionStorage;
using ShmImuWindow = MessageBuffer<ImuSample, 64, RegionStorage>;

}  // namespace

int main() {
  // Rule 1: the region is sized by the type, before anything exists.
  const std::size_t bytes = ShmImuWindow::StorageBytes();
  auto* region = static_cast<unsigned char*>(
      mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (region == MAP_FAILED) return 1;
  std::printf("region: %zu bytes for MessageBuffer<ImuSample, 64>\n", bytes);

  // Rule 2 (constructing side): the parent initializes the region ONCE,
  // before the fork, so both processes agree on a zeroed starting state.
  // (Across unrelated processes, xmMessaging's segment header plays this
  // role with a creation protocol; ad hoc, fork-after-init is the pattern.)
  { ShmImuWindow init(RegionStorage(region, bytes, /*initialize=*/true)); }

  const pid_t pid = fork();
  if (pid == 0) {
    // Child: the "sensor process". Attaches (no re-zero!) and stores.
    ShmImuWindow window(RegionStorage(region, bytes, /*initialize=*/false));
    for (std::uint64_t t = 1; t <= 100000; ++t) {
      window.Store(ImuSample::Make(t));
    }
    _exit(0);
  }

  // Parent: the "fusion process". Attaches the same region; snapshots race
  // the live child, then the final window survives the child's EXIT — the
  // data lives in the region, not in any process.
  ShmImuWindow window(RegionStorage(region, bytes, /*initialize=*/false));
  ImuSample snap[64];
  std::uint64_t torn = 0, live_windows = 0;
  while (waitpid(pid, nullptr, WNOHANG) == 0) {
    const std::size_t n = window.Snapshot(snap, 64);
    for (std::size_t i = 0; i < n; ++i) torn += snap[i].Torn() ? 1u : 0u;
    if (n > 0) ++live_windows;
  }
  const std::size_t final_n = window.Snapshot(snap, 64);

  std::printf(
      "parent: %lu live cross-process windows (torn %lu — must be 0); "
      "final window after child exit: %zu samples, newest t=%lu\n",
      static_cast<unsigned long>(live_windows),
      static_cast<unsigned long>(torn), final_n,
      static_cast<unsigned long>(final_n ? snap[0].t_us : 0));

  munmap(region, bytes);
  return (torn == 0 && final_n == 64 && snap[0].t_us == 100000) ? 0 : 1;
}
