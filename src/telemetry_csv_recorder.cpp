/*
 * telemetry_csv_recorder.cpp
 *
 * CsvSignalRecorder: the pre-MCAP recording-plane sink. Composes ON TOP of the
 * console binding — copies its table, then overrides get_signal/emit_signal so
 * the signal verb lands in per-channel CSV files while events/metrics/spans/
 * health stay on the console. See csv_signal_recorder.hpp for the contract.
 *
 * Mechanics:
 *  - The Binding table is raw C function pointers (no captures), so the two
 *    overridden entries are free-function TRAMPOLINES that reach the sole live
 *    recorder through a static instance pointer (g_active). At most one
 *    recorder exists at a time — the same process-singleton shape the console
 *    binding uses. Constructing a second aborts.
 *  - Channels have STABLE addresses (unique_ptr elements): get_signal hands the
 *    SDK a SignalSlot* fixed for the channel's life, and emit_signal resolves
 *    slot->id (1-based index) straight back to the channel — no per-emit lookup
 *    by name.
 *  - emit is mutex-guarded and synchronous (file write), thread-safe but not
 *    wait-free; a per-channel scratch row and one reused line buffer keep the
 *    steady state allocation-free.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmbase/telemetry/csv_signal_recorder.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "xmbase/telemetry/telemetry.hpp"

namespace xmotion {
namespace telemetry {

namespace {

// One recorded channel: its slot (SDK-facing id), schema, decoder, and file.
struct Channel {
  detail::SignalSlot slot;  // id = 1-based index into Impl::channels_
  std::string name;
  std::vector<std::string> columns;
  std::size_t payload_size = 0;
  std::function<void(const void*, std::vector<double>&)> decode;
  bool registered = false;  // Register<T>() supplied a schema
  bool warned = false;      // one-shot "no schema / size mismatch" warning
  std::ofstream file;
  bool header_written = false;
  std::vector<double> scratch;  // reused decode target (no per-emit alloc)
};

// Turn a channel name into a filesystem-safe stem: keep [A-Za-z0-9._-], map the
// rest (path separators, spaces) to '_' so "a/wheel 1" -> "a_wheel_1".
std::string SanitizeStem(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '-';
    out.push_back(keep ? c : '_');
  }
  if (out.empty()) out = "signal";
  return out;
}

}  // namespace

struct CsvSignalRecorder::Impl {
  explicit Impl(std::string dir) : directory(std::move(dir)) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
      XM_ERROR("csv_recorder: cannot create '{}': {}", directory,
               ec.message());
    }
    BuildBinding();
  }

  // Delegate the whole table to the console binding, then claim the two signal
  // entries. If logging is compiled out (no console binding), fill the
  // delegated entries with safe stubs so the table is still complete.
  void BuildBinding() {
    if (const Binding* base = detail::DefaultConsoleBinding()) {
      binding = *base;
    } else {
      binding = Binding{};
      binding.abi_version = kBindingAbiVersion;
      binding.get_counter = [](std::string_view) {
        return &detail::g_noop_counter;
      };
      binding.get_gauge = [](std::string_view) {
        return &detail::g_noop_gauge;
      };
      binding.get_histogram = [](std::string_view) {
        return &detail::g_noop_histogram;
      };
      binding.intern_source = [](std::string_view) { return 0u; };
      binding.should_log = [](Severity) noexcept { return false; };
      binding.set_level = [](Severity) noexcept {};
      binding.get_level = []() noexcept { return Severity::kOff; };
      binding.emit_event = [](std::uint32_t, Severity, const char*,
                              const detail::ArgPack&, Context,
                              Timestamp) noexcept {};
      binding.emit_event_dyn = [](std::uint32_t, Severity, const char*,
                                  std::size_t, Context, Timestamp) noexcept {};
      binding.emit_span = [](const char*, Context, SpanId, Timestamp, Timestamp,
                             const Context*, std::uint8_t) noexcept {};
      binding.report_health = [](const char*, HealthState, const char*, Context,
                                 Timestamp) noexcept {};
      binding.set_resource = [](std::string_view, std::string_view) {};
    }
    binding.get_signal = &GetSignalTrampoline;
    binding.emit_signal = &EmitSignalTrampoline;
  }

  // --- registration path (init-time, non-RT) --------------------------------

  // Find channel by name, or create it (unregistered) with the next id.
  Channel& FindOrCreate(std::string_view name) {
    auto it = by_name.find(std::string(name));
    if (it != by_name.end()) return *it->second;
    auto ch = std::make_unique<Channel>();
    ch->name = std::string(name);
    ch->slot.id = static_cast<std::uint32_t>(channels.size() + 1);  // 1-based
    Channel* raw = ch.get();
    channels.push_back(std::move(ch));
    by_name.emplace(raw->name, raw);
    return *raw;
  }

  void Register(std::string_view name, std::vector<std::string> columns,
                std::size_t payload_size,
                std::function<void(const void*, std::vector<double>&)> decode) {
    std::lock_guard<std::mutex> lock(mtx);
    Channel& ch = FindOrCreate(name);
    ch.columns = std::move(columns);
    ch.payload_size = payload_size;
    ch.decode = std::move(decode);
    ch.scratch.reserve(ch.columns.size());
    ch.registered = true;
  }

  detail::SignalSlot* GetSignal(std::string_view name, std::size_t size) {
    std::lock_guard<std::mutex> lock(mtx);
    Channel& ch = FindOrCreate(name);
    if (ch.registered && ch.payload_size != size) {
      XM_WARN("csv_recorder: channel '{}' payload size {} != registered {}",
              ch.name, size, ch.payload_size);
    }
    return &ch.slot;
  }

  // --- emit path (per-sample; mutex-guarded, synchronous) -------------------

  void EmitSignal(detail::SignalSlot* slot, const void* bytes, std::size_t size,
                  Timestamp ts) {
    if (slot == nullptr) return;
    std::lock_guard<std::mutex> lock(mtx);
    const std::uint32_t id = slot->id;
    if (id == 0 || id > channels.size()) {
      ++dropped;
      return;
    }
    Channel& ch = *channels[id - 1];
    if (!ch.registered || !ch.decode || size != ch.payload_size) {
      if (!ch.warned) {
        XM_WARN("csv_recorder: dropping '{}' — {}", ch.name,
                ch.registered
                    ? "payload size mismatch"
                    : "no schema (call Register<T>() before install)");
        ch.warned = true;
      }
      ++dropped;
      return;
    }
    if (!EnsureOpen(ch)) {
      ++dropped;
      return;
    }

    ch.scratch.clear();
    ch.decode(bytes, ch.scratch);

    // "<ts_ns>,<col0>,<col1>,...\n" built into one reused buffer.
    line.clear();
    char num[32];
    std::snprintf(num, sizeof num, "%lld",
                  static_cast<long long>(ts.time_since_epoch().count()));
    line += num;
    for (double v : ch.scratch) {
      std::snprintf(num, sizeof num, "%.9g", v);
      line += ',';
      line += num;
    }
    line += '\n';
    ch.file.write(line.data(), static_cast<std::streamsize>(line.size()));
    ++written;
  }

  // Open the channel file (once) and write "ts_ns,<cols>". False on open error.
  bool EnsureOpen(Channel& ch) {
    if (ch.header_written) return ch.file.good();
    const std::filesystem::path path =
        std::filesystem::path(directory) / (SanitizeStem(ch.name) + ".csv");
    ch.file.open(path, std::ios::out | std::ios::trunc);
    if (!ch.file) {
      XM_ERROR("csv_recorder: cannot open '{}'", path.string());
      ch.header_written = true;  // don't retry every sample
      return false;
    }
    ch.file << "ts_ns";
    for (const std::string& c : ch.columns) ch.file << ',' << c;
    ch.file << '\n';
    ch.header_written = true;
    return true;
  }

  std::string directory;
  std::mutex mtx;
  std::vector<std::unique_ptr<Channel>> channels;  // index = id - 1
  std::unordered_map<std::string, Channel*> by_name;
  std::string line;  // reused emit-line buffer
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> dropped{0};
  Binding binding{};

  // Trampolines: the Binding holds plain function pointers, so these reach the
  // sole live recorder through g_active (set in the recorder ctor).
  static Impl* g_active;
  static detail::SignalSlot* GetSignalTrampoline(std::string_view name,
                                                 std::size_t size, std::size_t,
                                                 const char*) {
    return g_active != nullptr ? g_active->GetSignal(name, size) : nullptr;
  }
  static void EmitSignalTrampoline(detail::SignalSlot* slot, const void* bytes,
                                   std::size_t size, Timestamp ts) noexcept {
    if (g_active != nullptr) g_active->EmitSignal(slot, bytes, size, ts);
  }
};

CsvSignalRecorder::Impl* CsvSignalRecorder::Impl::g_active = nullptr;

CsvSignalRecorder::CsvSignalRecorder(std::string directory)
    : impl_(std::make_unique<Impl>(std::move(directory))) {
  if (Impl::g_active != nullptr) {
    XM_FATAL(
        "csv_recorder: a CsvSignalRecorder is already active (the binding is a "
        "process singleton) — destroy it before constructing another");
    std::abort();
  }
  Impl::g_active = impl_.get();
}

CsvSignalRecorder::~CsvSignalRecorder() {
  // Reverting the active binding is the owner's job; here we just stop being
  // reachable and flush every file.
  if (Impl::g_active == impl_.get()) Impl::g_active = nullptr;
  for (auto& ch : impl_->channels) {
    if (ch->file.is_open()) ch->file.close();
  }
}

void CsvSignalRecorder::RegisterErased(
    std::string_view name, std::vector<std::string> columns,
    std::size_t payload_size,
    std::function<void(const void*, std::vector<double>&)> decode) {
  impl_->Register(name, std::move(columns), payload_size, std::move(decode));
}

const Binding* CsvSignalRecorder::binding() const noexcept {
  return &impl_->binding;
}

std::uint64_t CsvSignalRecorder::samples_written() const noexcept {
  return impl_->written.load(std::memory_order_relaxed);
}

std::uint64_t CsvSignalRecorder::samples_dropped() const noexcept {
  return impl_->dropped.load(std::memory_order_relaxed);
}

}  // namespace telemetry
}  // namespace xmotion
