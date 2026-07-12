# Unit tests for the xmBase module.
#
# Each test links the library target it exercises plus gtest_main and is
# registered with ctest via gtest_discover_tests so `ctest` actually runs
# them. Core-tier tests link xmBase only (they must build with no Eigen);
# geometry-tier tests link xmBaseGeometry and are skipped in core-only
# builds (-DXMBASE_GEOMETRY=OFF).
add_executable(test_logging test_logging.cpp)
target_link_libraries(test_logging xmBase gtest gtest_main)
gtest_discover_tests(test_logging)

add_executable(test_types_core test_types_core.cpp)
target_link_libraries(test_types_core xmBase gtest gtest_main)
gtest_discover_tests(test_types_core)

add_executable(test_event test_event.cpp)
target_link_libraries(test_event xmBase gtest gtest_main)
gtest_discover_tests(test_event)

add_executable(test_ring_buffer test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer xmBase gtest gtest_main)
gtest_discover_tests(test_ring_buffer)

add_executable(test_serialization test_serialization.cpp)
target_link_libraries(test_serialization xmBase gtest gtest_main)
gtest_discover_tests(test_serialization)

add_executable(test_telemetry_api test_telemetry_api.cpp)
target_link_libraries(test_telemetry_api xmBase gtest gtest_main)
gtest_discover_tests(test_telemetry_api)

# Concurrency primitive verification (ADR 0007 W1) — suite names carry
# "Concurrency" so the CI TSan leg can select them with a ctest filter.
add_executable(test_concurrency_latest_slot test_concurrency_latest_slot.cpp)
target_link_libraries(test_concurrency_latest_slot xmBase gtest gtest_main)
gtest_discover_tests(test_concurrency_latest_slot)

add_executable(test_concurrency_bounded_queue test_concurrency_bounded_queue.cpp)
target_link_libraries(test_concurrency_bounded_queue xmBase gtest gtest_main)
gtest_discover_tests(test_concurrency_bounded_queue)

add_executable(test_concurrency_waiter test_concurrency_waiter.cpp)
target_link_libraries(test_concurrency_waiter xmBase gtest gtest_main)
gtest_discover_tests(test_concurrency_waiter)

add_executable(test_concurrency_placement test_concurrency_placement.cpp)
target_link_libraries(test_concurrency_placement xmBase gtest gtest_main)
gtest_discover_tests(test_concurrency_placement)

# Geometry tier (Eigen) tests.
if (TARGET xmBaseGeometry)
  add_executable(test_types test_types.cpp)
  target_link_libraries(test_types xmBaseGeometry gtest gtest_main)
  gtest_discover_tests(test_types)
endif ()
