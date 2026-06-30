# Unit tests for the xmSigma module.
#
# Each test links the xmSigma library plus gtest_main and is registered with
# ctest via gtest_discover_tests so `ctest` actually runs them.
add_executable(test_csv_logger test_csv_logger.cpp)
target_link_libraries(test_csv_logger xmSigma gtest gtest_main)
gtest_discover_tests(test_csv_logger)

add_executable(test_xlogger test_xlogger.cpp)
target_link_libraries(test_xlogger xmSigma gtest gtest_main)
gtest_discover_tests(test_xlogger)

add_executable(test_event_logger test_event_logger.cpp)
target_link_libraries(test_event_logger xmSigma gtest gtest_main)
gtest_discover_tests(test_event_logger)

add_executable(test_ctrl_logger test_ctrl_logger.cpp)
target_link_libraries(test_ctrl_logger xmSigma gtest gtest_main)
gtest_discover_tests(test_ctrl_logger)

add_executable(test_rt_logger test_rt_logger.cpp)
target_link_libraries(test_rt_logger xmSigma gtest gtest_main)
gtest_discover_tests(test_rt_logger)

add_executable(test_mpsc_rt_logger test_mpsc_rt_logger.cpp)
target_link_libraries(test_mpsc_rt_logger xmSigma gtest gtest_main)
gtest_discover_tests(test_mpsc_rt_logger)

add_executable(test_types test_types.cpp)
target_link_libraries(test_types xmSigma gtest gtest_main)
gtest_discover_tests(test_types)
