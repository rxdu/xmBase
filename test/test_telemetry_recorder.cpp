/*
 * test_telemetry_recorder.cpp
 *
 * CsvSignalRecorder: the signal verb records to per-channel CSV while every
 * other verb delegates to the console binding. Covers the register->publish->
 * read-back round trip, the drop path (unregistered / size-mismatched channel),
 * and event delegation.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmbase/telemetry/csv_signal_recorder.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;

namespace {

// A steering-response-shaped POD (mirrors the app's sample record).
struct Wheel {
  double cmd_deg;
  double pos_deg;
  std::int32_t phase;
};

// A per-test unique directory under the gtest temp root.
std::filesystem::path FreshDir(const std::string& tag) {
  static int counter = 0;
  std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) /
                              ("xmbase_rec_" + tag + "_" +
                               std::to_string(counter++));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return dir;
}

// Split a CSV file into rows of fields (first row is the header).
std::vector<std::vector<std::string>> ReadCsv(
    const std::filesystem::path& path) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(field);
    rows.push_back(std::move(fields));
  }
  return rows;
}

}  // namespace

TEST(CsvSignalRecorder, RecordsRegisteredChannelToCsv) {
  const std::filesystem::path dir = FreshDir("roundtrip");
  {
    tel::CsvSignalRecorder rec(dir.string());
    rec.Register<Wheel>(
        "motion.wheel", {"cmd_deg", "pos_deg", "phase"},
        [](const Wheel& w, std::vector<double>& row) {
          row = {w.cmd_deg, w.pos_deg, static_cast<double>(w.phase)};
        });
    ASSERT_TRUE(tel::InstallBinding(rec.binding()));

    auto ch = tel::GetChannel<Wheel>("motion.wheel");
    ch.Publish(Wheel{10.0, 9.5, 1});
    ch.Publish(Wheel{20.0, 19.25, 2});

    tel::InstallBinding(nullptr);  // unbind BEFORE the recorder is destroyed
    EXPECT_EQ(rec.samples_written(), 2u);
    EXPECT_EQ(rec.samples_dropped(), 0u);
  }  // recorder destroyed -> files flushed and closed

  const auto rows = ReadCsv(dir / "motion.wheel.csv");
  ASSERT_EQ(rows.size(), 3u);  // header + 2 samples
  EXPECT_EQ(rows[0],
            (std::vector<std::string>{"ts_ns", "cmd_deg", "pos_deg", "phase"}));
  // ts_ns is monotonic-clock-dependent; assert only the decoded columns.
  ASSERT_EQ(rows[1].size(), 4u);
  EXPECT_EQ(rows[1][1], "10");
  EXPECT_EQ(rows[1][2], "9.5");
  EXPECT_EQ(rows[1][3], "1");
  EXPECT_EQ(rows[2][1], "20");
  EXPECT_EQ(rows[2][2], "19.25");
  EXPECT_EQ(rows[2][3], "2");
  // Timestamps are non-decreasing.
  EXPECT_LE(std::stoll(rows[1][0]), std::stoll(rows[2][0]));
}

TEST(CsvSignalRecorder, DropsUnregisteredChannel) {
  const std::filesystem::path dir = FreshDir("unregistered");
  tel::CsvSignalRecorder rec(dir.string());
  ASSERT_TRUE(tel::InstallBinding(rec.binding()));

  auto ch = tel::GetChannel<Wheel>("no.schema");
  ch.Publish(Wheel{1.0, 2.0, 3});
  ch.Publish(Wheel{4.0, 5.0, 6});

  tel::InstallBinding(nullptr);
  EXPECT_EQ(rec.samples_written(), 0u);
  EXPECT_EQ(rec.samples_dropped(), 2u);
  EXPECT_FALSE(std::filesystem::exists(dir / "no.schema.csv"));
}

TEST(CsvSignalRecorder, EventsStillDelegateToConsole) {
  const std::filesystem::path dir = FreshDir("delegate");
  tel::CsvSignalRecorder rec(dir.string());
  ASSERT_TRUE(tel::InstallBinding(rec.binding()));

  testing::internal::CaptureStderr();
  XM_WARN("recorder delegate check {}", 42);
  const std::string err = testing::internal::GetCapturedStderr();

  tel::InstallBinding(nullptr);
  EXPECT_NE(err.find("recorder delegate check 42"), std::string::npos);
}
