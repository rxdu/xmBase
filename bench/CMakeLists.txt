# Primitive benchmarks for the xmBase module (xmbase/testing harness).
#
# The bench binary links only the CORE (Eigen-free) target — the primitives
# it measures must never drag the geometry tier in. A scaled smoke run is
# registered with ctest so CI proves the binary runs and the alloc gate holds;
# full runs (and their pinned JSON reports) are executed manually:
#
#   ./bin/xmbase_bench --out xmbase_bench_report.json
add_executable(xmbase_bench xmbase_bench.cpp)
target_link_libraries(xmbase_bench xmBase)

add_test(NAME xmbase_bench_smoke
    COMMAND xmbase_bench --smoke --out ${CMAKE_CURRENT_BINARY_DIR}/xmbase_bench_smoke.json)
