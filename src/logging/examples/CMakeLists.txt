# Example programs for the logging module.
#
# These are runnable demonstrations of the logging API (they have a main() and
# no assertions). They are NOT unit tests and are intentionally not registered
# with ctest. The gtest-based unit tests live in ../test.
add_executable(ctrl_logger_example ctrl_logger_example.cpp)
target_link_libraries(ctrl_logger_example logging)

add_executable(xlogger_example xlogger_example.cpp)
target_link_libraries(xlogger_example logging)
