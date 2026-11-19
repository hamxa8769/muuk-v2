add_library(muuk_core STATIC
    src/lot_progression.cpp
    src/market_brain.cpp
    src/market_brain_v2.cpp
    src/market_read.cpp
    src/pnl.cpp
    src/regime_range.cpp
    src/risk.cpp
    src/risk_table.cpp
    src/strategy_engine.cpp
)

target_include_directories(muuk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(muuk_core PUBLIC cxx_std_20)

# Brain v2 trainer: trains and walk-forwards v2 on MT5 one-minute history.
# Core-only (no FIX, no JSON, no desktop), so it builds on Linux and Windows.
add_executable(muuk_brain_v2_trainer
    cli/brain_v2_main.cpp
)
target_link_libraries(muuk_brain_v2_trainer PRIVATE muuk_core)

if(MUUK_BUILD_TESTS)
    add_executable(muuk_core_tests
        tests/test_types.cpp
        tests/test_lot_progression.cpp
        tests/test_market_brain.cpp
        tests/test_market_brain_v2.cpp
        tests/test_market_read.cpp
        tests/test_pnl.cpp
        tests/test_regime_range.cpp
        tests/test_risk.cpp
        tests/test_strategy_engine.cpp
    )
    target_link_libraries(muuk_core_tests PRIVATE muuk_core Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(muuk_core_tests)
endif()

if(MUUK_BUILD_BENCHMARKS)
    add_executable(muuk_core_bench
        bench/bench_strategy_engine.cpp
    )
    target_link_libraries(muuk_core_bench PRIVATE muuk_core benchmark::benchmark)
endif()
