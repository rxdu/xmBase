# Unit tests for the xmBase module.
#
# Each test links the xmBase library plus gtest_main and is registered with
# ctest via gtest_discover_tests so `ctest` actually runs them.
add_executable(test_logging test_logging.cpp)
target_link_libraries(test_logging xmBase gtest gtest_main)
gtest_discover_tests(test_logging)

add_executable(test_types test_types.cpp)
target_link_libraries(test_types xmBase gtest gtest_main)
gtest_discover_tests(test_types)

add_executable(test_event test_event.cpp)
target_link_libraries(test_event xmBase gtest gtest_main)
gtest_discover_tests(test_event)

add_executable(test_math_utils test_math_utils.cpp)
target_link_libraries(test_math_utils xmBase gtest gtest_main)
gtest_discover_tests(test_math_utils)

add_executable(test_ring_buffer test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer xmBase gtest gtest_main)
gtest_discover_tests(test_ring_buffer)

add_executable(test_serialization test_serialization.cpp)
target_link_libraries(test_serialization xmBase gtest gtest_main)
gtest_discover_tests(test_serialization)

add_executable(test_telemetry_api test_telemetry_api.cpp)
target_link_libraries(test_telemetry_api xmBase gtest gtest_main)
gtest_discover_tests(test_telemetry_api)
