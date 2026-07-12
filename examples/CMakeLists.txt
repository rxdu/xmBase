# Example programs for the xmBase module.
#
# These are runnable demonstrations of the xmBase API (they have a main() and
# no assertions). They are NOT unit tests and are intentionally not registered
# with ctest. The gtest-based unit tests live in ../test.

add_executable(xm_logging_example xm_logging_example.cpp)
target_link_libraries(xm_logging_example xmBase)



# types_example demonstrates the geometry tier (Eigen) — skipped in
# core-only builds (-DXMBASE_GEOMETRY=OFF).
if (TARGET xmBaseGeometry)
  add_executable(types_example types_example.cpp)
  target_link_libraries(types_example xmBaseGeometry)
endif ()

add_executable(instrumented_control_loop_example instrumented_control_loop_example.cpp)
target_link_libraries(instrumented_control_loop_example xmBase)

add_executable(trace_pipeline_example trace_pipeline_example.cpp)
target_link_libraries(trace_pipeline_example xmBase)

add_executable(device_health_example device_health_example.cpp)
target_link_libraries(device_health_example xmBase)

add_executable(custom_binding_example custom_binding_example.cpp)
target_link_libraries(custom_binding_example xmBase)

add_executable(worker_pool_context_example worker_pool_context_example.cpp)
target_link_libraries(worker_pool_context_example xmBase)

# The concurrency examples double as the migration guide from the
# deprecated container/ and event/ modules (see CHANGELOG 0.5.0):
#   message_slot — the latest-value handoff (what ring_buffer was bent into)
#   worker_inbox — the bounded async-dispatch pattern (what event/ +
#                  thread_safe_queue did, with explicit overflow policy)
add_executable(concurrency_message_slot_example concurrency_message_slot_example.cpp)
target_link_libraries(concurrency_message_slot_example xmBase)

add_executable(concurrency_worker_inbox_example concurrency_worker_inbox_example.cpp)
target_link_libraries(concurrency_worker_inbox_example xmBase)
