# Unit tests for the logging module.
#
# Each test links the logging library plus gtest_main and is registered with
# ctest via gtest_discover_tests so `ctest` actually runs them.
add_executable(test_csv_logger test_csv_logger.cpp)
target_link_libraries(test_csv_logger logging gtest gtest_main)
gtest_discover_tests(test_csv_logger)

add_executable(test_xlogger test_xlogger.cpp)
target_link_libraries(test_xlogger logging gtest gtest_main)
gtest_discover_tests(test_xlogger)
