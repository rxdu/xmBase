# Example programs for the xmBase module.
#
# These are runnable demonstrations of the xmBase API (they have a main() and
# no assertions). They are NOT unit tests and are intentionally not registered
# with ctest. The gtest-based unit tests live in ../test.
add_executable(ctrl_logger_example ctrl_logger_example.cpp)
target_link_libraries(ctrl_logger_example xmBase)

add_executable(xlogger_example xlogger_example.cpp)
target_link_libraries(xlogger_example xmBase)

add_executable(rt_logger_example rt_logger_example.cpp)
target_link_libraries(rt_logger_example xmBase)

add_executable(mpsc_rt_logger_example mpsc_rt_logger_example.cpp)
target_link_libraries(mpsc_rt_logger_example xmBase)

add_executable(types_example types_example.cpp)
target_link_libraries(types_example xmBase)
