/*
 * ctrl_logger_example.cpp
 *
 * Created on: Oct 6, 2016
 * Description: Example program demonstrating CtrlLogger, CsvLogger and
 *              EventLogger usage. This is an example, NOT a unit test
 *              (it has no assertions); see ../test/ for the gtest-based
 *              unit tests.
 *
 * Copyright (c) 2018 Ruixiang Du (rdu)
 */

#include <memory>
#include <string>

#include "xmsigma/logging/ctrl_logger.hpp"
#include "xmsigma/logging/csv_logger.hpp"
#include "xmsigma/logging/event_logger.hpp"
#include "xmsigma/logging/details/logging_utils.hpp"

using namespace xmotion;

int main(int argc, char* argv[]) {
  // Use a portable, HOME-based log location instead of a hardcoded path.
  const std::string log_path = GetLogFolderPath();

  CtrlLogger& ctrl_logger = CtrlLogger::GetLogger("ctrl_log", log_path);
  ctrl_logger.AddItemNameToEntryHead("x1");
  ctrl_logger.AddItemNameToEntryHead("x2");
  ctrl_logger.AddItemNameToEntryHead("x3");
  ctrl_logger.AddItemNameToEntryHead("x4");
  ctrl_logger.PassEntryHeaderToLogger();

  for (int i = 0; i < 500; i++) {
    ctrl_logger.AddItemDataToEntry("x1", i + 1);
    ctrl_logger.AddItemDataToEntry("x2", i + 2);
    ctrl_logger.AddItemDataToEntry("x3", i + 3);
    ctrl_logger.AddItemDataToEntry("x4", i + 4);

    ctrl_logger.PassEntryDataToLogger();
  }

  CsvLogger csv_logger("csv_log", log_path);

  for (int i = 0; i < 500; i++) {
    csv_logger.LogData(i + 1, i + 0.5, i * 100.5);

    // global csv logger can be called anywhere within the process
    GlobalCsvLogger::GetLogger("global_csv_log", log_path).LogData(21, 34, 56);
  }

  EventLogger event_logger("event_log", log_path);

  for (int i = 0; i < 500; i++)
    event_logger.LogEvent("event1", "event2", i * 100.5);

  return 0;
}
