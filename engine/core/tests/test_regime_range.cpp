#include <cmath>
#include <deque>
#include <variant>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/core/regime_range.hpp"
#include "muuk/core/strategy_engine.hpp"

using namespace muuk::core;

namespace {

// Minutes swinging around a level: no direction.
std::deque<Bar> sideways(int count = 240, double center = 4340.0, double swing = 6.0) {
    std::deque<Bar> out;
    double prev = center;
    for (int i = 0; i < count; ++i) {
        const double c = center + swing * std::sin(i * 0.9);
        out.push_back(Bar{i * 60'000LL, prev, std::max(prev, c) + 0.3, std::min(prev, c) - 0.3, c, 50.0});
        prev = c;
    }
    return out;
}

std::deque<Bar> rising(int count = 240, double perBar = 1.0) {
    std::deque<Bar> out;
    double prev = 4000.0;
    for (int i = 0; i < count; ++i) {
        const double c = prev + perBar;
        out.push_back(Bar{i * 60'000LL, prev, c + 0.2, prev - 0.2, c, 50.0});
        prev = c;
    }
    return out;
}

RegimeRead readWith(double atr, double high, double low) {
    RegimeRead r;
    r.regime = MarketRegime::Ranging;
    r.atr = atr;
    r.atrMinute = atr;
    r.choppiness = 61.8;  // the whole high-to-low
    r.recentHigh = high;
    r.recentLow = low;
    r.bollingerUpper = high;
    r.bollingerLower = low;
    return r;
}

}  // namespace

TEST_CASE("Regime indicators: the textbook values on plain bars") {
    const std::deque<Bar> up = aggregateBars(rising(), 5);
    REQUIRE(up.size() == 48);
    REQUIRE(up[0].open == Catch::Approx(4000.0));
    REQUIRE(up[0].close == Catch::Approx(4005.0));
    REQUIRE(up[0].tickVolume == Catch::Approx(250.0));
    REQUIRE(rsiOf(up, 14) == Catch::Approx(100.0));         // it only ever rose
    REQUIRE(adxOf(up, 14) == Catch::Approx(100.0));         // all direction
    REQUIRE(choppinessOf(up, 14) < 35.0);
    REQUIRE(smaOf(up, 2) == Catch::Approx((up[46].close + up[47].close) / 2.0));

    std::deque<Bar> flat;
    for (int i = 0; i < 40; ++i) flat.push_back(Bar{i * 300'000LL, 100.0, 101.0, 99.0, 100.0, 1.0});
    REQUIRE(atrOf(flat, 14) == Catch::Approx(2.0));
    REQUIRE(rsiOf(flat, 14) == Catch::Approx(50.0));
    REQUIRE(emaOf(flat, 8) == Catch::Approx(100.0));
}

TEST_CASE("Regime: no cycle without enough bars; a steady rise is trending, swings are ranging") {
    RegimeConfig cfg;
    REQUIRE(readRegime(sideways(60), cfg).regime == MarketRegime::Unknown);
    REQUIRE(readRegime(rising(), cfg).regime == MarketRegime::Trending);
    const RegimeRead swings = readRegime(sideways(), cfg);
    INFO(swings.why);
    REQUIRE(swings.regime == MarketRegime::Ranging);
}

TEST_CASE("Regime range: the orders are outside the range and their distance stays within the limits") {
    RegimeConfig cfg;
    cfg.minGap = 25.0;
    cfg.maxGap = 150.0;
    SECTION("too narrow: raised to the minimum gap") {
        const RegimeRange r = computeRegimeRange(readWith(1.0, 4341.0, 4339.0), 4340.0, {}, 0.0, 0.0, cfg);
        REQUIRE(r.buyStop - r.sellStop == Catch::Approx(25.0));
        REQUIRE(r.clamped);
        REQUIRE(r.buyStop > r.high);
        REQUIRE(r.sellStop < r.low);
    }
    SECTION("too wide: cut to the maximum gap, the price still between the orders") {
        const RegimeRange r = computeRegimeRange(readWith(5.0, 4600.0, 4200.0), 4590.0, {}, 0.0, 0.0, cfg);
        REQUIRE(r.buyStop - r.sellStop == Catch::Approx(150.0));
        REQUIRE(r.buyStop > 4590.0);
        REQUIRE(r.sellStop < 4590.0);
    }
    SECTION("in between: the data decides") {
        const RegimeRange r = computeRegimeRange(readWith(4.0, 4380.0, 4320.0), 4350.0, {}, 0.0, 0.0, cfg);
        REQUIRE_FALSE(r.clamped);
        REQUIRE(r.buyStop == Catch::Approx(4382.0));
        REQUIRE(r.sellStop == Catch::Approx(4318.0));
    }
    SECTION("the price at the edge of a minimum-gap range: both orders move, the gap stays") {
        const RegimeRange r = computeRegimeRange(readWith(0.0, 4341.0, 4339.0), 4352.4, {}, 0.0, 0.0, cfg);
        REQUIRE(r.buyStop - r.sellStop == Catch::Approx(25.0));
        REQUIRE(r.buyStop >= 4352.9 - 1e-9);
    }
}

TEST_CASE("Regime range: the walls and the basket's trades widen it, walls too far away don't") {
    RegimeConfig cfg;
    cfg.minGap = 25.0;
    cfg.maxGap = 150.0;
    const RegimeRead read = readWith(0.0, 4380.0, 4320.0);
    ExternalLevels levels;
    levels.askWall = 4390.0;
    levels.putWall = 4310.0;
    RegimeRange r = computeRegimeRange(read, 4350.0, levels, 0.0, 0.0, cfg);
    REQUIRE(r.buyStop == Catch::Approx(4390.0));
    REQUIRE(r.sellStop == Catch::Approx(4310.0));

    levels.askWall = 4600.0;  // beyond half the maximum gap
    levels.putWall = 0.0;
    r = computeRegimeRange(read, 4350.0, levels, 0.0, 0.0, cfg);
    REQUIRE(r.buyStop == Catch::Approx(4380.0));

    r = computeRegimeRange(read, 4350.0, {}, 4315.0, 4385.0, cfg);
    REQUIRE(r.buyStop == Catch::Approx(4385.0));
    REQUIRE(r.sellStop == Catch::Approx(4315.0));
}

// NOTE (Brain v2 merge): the StrategyEngine::moveRange test from the worktree
// is omitted here: moveRange does not exist in this tree's StrategyEngine.
// Live regime wiring into LiveRunner is a separate step; see BRAIN_UPGRADE.md.
