/*
 * telemetry/csv_signal_recorder.hpp
 *
 * A recording-plane sink for the `signal` verb: captures high-rate typed
 * SignalChannel<T> samples to CSV (one file per channel) while DELEGATING every
 * other verb — events, metrics, spans, health, level control — to the built-in
 * console binding. Install it and the signal plane starts recording; logging is
 * unchanged.
 *
 *   CsvSignalRecorder rec("out_dir");
 *   rec.Register<Sample>("motion.wheel",
 *                        {"cmd_deg", "pos_deg", "speed"},
 *                        [](const Sample& s, std::vector<double>& row) {
 *                          row = {s.cmd_deg, s.pos_deg, s.speed};
 *                        });
 *   telemetry::InstallBinding(rec.binding());       // recording plane live
 *   GetChannel<Sample>("motion.wheel").Publish(s);  // -> motion.wheel.csv
 *
 * Why an explicit decoder: emit_signal is type-erased (raw bytes + size), so
 * the recorder cannot know a POD's fields. Register<T>() supplies the column
 * names and a typed extractor; the template folds T away, and the recorder
 * writes "<ts_ns>,<col0>,<col1>,..." per Publish, header row first.
 *
 * Scope: this is the pre-MCAP recording sink (delta P3 lands the
 * self-describing MCAP channel story). Emit does a mutex-guarded file write —
 * thread-safe and synchronous like the console binding, NOT wait-free. It is a
 * bench/diagnostic recorder (bring-up characterization, response capture), not
 * a hard-RT hot-path sink; keep publish rates to the low kHz a file sink can
 * absorb.
 *
 * Single-instance: the active binding is a process singleton, so at most one
 * CsvSignalRecorder may exist at a time (constructing a second aborts). Destroy
 * it (which flushes/closes every file) before installing another binding.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "xmbase/telemetry/binding.hpp"

namespace xmotion {
namespace telemetry {

class CsvSignalRecorder {
 public:
  // Records into `directory` (created if absent); one file per channel.
  explicit CsvSignalRecorder(std::string directory);
  ~CsvSignalRecorder();

  CsvSignalRecorder(const CsvSignalRecorder&) = delete;
  CsvSignalRecorder& operator=(const CsvSignalRecorder&) = delete;

  // Declare how channel `name`'s POD decodes into CSV columns. Call BEFORE
  // InstallBinding()/Publish (registration is init-time, per the contract).
  // `extract` receives each sample and fills `row` with one value per column
  // (in `columns` order). Re-registering the same name replaces the schema.
  template <typename T>
  void Register(std::string_view name, std::vector<std::string> columns,
                std::function<void(const T&, std::vector<double>&)> extract) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "signal payloads must be trivially copyable PODs (D4)");
    RegisterErased(
        name, std::move(columns), sizeof(T),
        [fn = std::move(extract)](const void* bytes,
                                  std::vector<double>& out) {
          fn(*static_cast<const T*>(bytes), out);
        });
  }

  // The composed binding: signals recorded here, everything else delegated to
  // the console binding. Pass to InstallBinding(); the pointer stays valid for
  // the recorder's lifetime.
  const Binding* binding() const noexcept;

  // Diagnostics. dropped = samples published on a channel with no registered
  // schema, or whose payload size disagrees with the registration.
  std::uint64_t samples_written() const noexcept;
  std::uint64_t samples_dropped() const noexcept;

 private:
  void RegisterErased(
      std::string_view name, std::vector<std::string> columns,
      std::size_t payload_size,
      std::function<void(const void*, std::vector<double>&)> decode);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace telemetry
}  // namespace xmotion
