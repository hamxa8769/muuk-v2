#include <cmath>
#include <deque>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "muuk/core/market_brain.hpp"
#include "muuk/core/regime_range.hpp"

using namespace muuk::core;

namespace {

Bar bar(int i, double high, double low, double close) { return Bar{i * 60'000LL, close, high, low, close, 10.0}; }

std::vector<Bar> sideways(int count, double swing = 3.0) {
    std::vector<Bar> out;
    double prev = 4000.0;
    for (int i = 0; i < count; ++i) {
        const double c = 4000.0 + swing * std::sin(i * 0.05) + 0.5 * std::sin(i * 1.3);
        out.push_back(Bar{i * 60'000LL, prev, std::max(prev, c) + 0.2, std::min(prev, c) - 0.2, c, 20.0});
        prev = c;
    }
    return out;
}

}  // namespace

TEST_CASE("Brain: the gap is always within $2 and $15") {
    BrainConfig cfg;
    REQUIRE(clampedGap(2.0, 0.1, cfg) == Catch::Approx(2.0));
    REQUIRE(clampedGap(8.0, 5.0, cfg) == Catch::Approx(15.0));
    REQUIRE(clampedGap(3.0, 2.0, cfg) == Catch::Approx(6.0));
}

TEST_CASE("Brain replay: nothing filled is nothing; a run past the line reaches the target") {
    BrainConfig cfg;
    SECTION("no line touched") {
        const std::vector<Bar> bars{bar(0, 100, 100, 100), bar(1, 100.5, 99.5, 100), bar(2, 100.5, 99.5, 100)};
        const ReplayCycle r = replayCycle(bars, {}, 0, 4.0, 1.0, cfg);
        REQUIRE(r.fills == 0);
        REQUIRE(r.pnl == 0.0);
    }
    SECTION("buy at 102, then 104: 1 lot x half the gap") {
        const std::vector<Bar> bars{bar(0, 100, 100, 100), bar(1, 102.5, 100, 102.2), bar(2, 104.5, 102, 104)};
        const ReplayCycle r = replayCycle(bars, {}, 0, 4.0, 1.0, cfg);
        REQUIRE(r.fills == 1);
        REQUIRE(r.pnl == Catch::Approx(2.0));
        REQUIRE_FALSE(r.locked);
    }
    SECTION("the spread is charged per lot") {
        const std::vector<Bar> bars{bar(0, 100, 100, 100), bar(1, 102.5, 100, 102.2), bar(2, 102.3, 102.1, 102.2)};
        const ReplayCycle r = replayCycle(bars, {0.0, 0.3, 0.3}, 0, 4.0, 1.0, cfg);
        REQUIRE(r.pnl == Catch::Approx(0.2 - 0.3));
    }
}

TEST_CASE("Brain replay: whipsaws reach 8 positions, then the lock freezes the loss") {
    BrainConfig cfg;
    cfg.stepAtr = 0.0;  // the lines stay at 102 / 98
    std::vector<Bar> bars{bar(0, 100, 100, 100)};
    for (int i = 1; i <= 12; ++i) {
        bars.push_back(i % 2 ? bar(i, 102.1, 100, 102) : bar(i, 100, 97.9, 98));
    }
    const ReplayCycle r = replayCycle(bars, {}, 0, 4.0, 0.0, cfg);
    REQUIRE(r.locked);
    REQUIRE(r.fills == 8);
    // Buys 1+4+16+64 = 85 at 102, sells 2+8+32+128 = 170 at 98; the 9th (a buy) is the lock, at 102.
    REQUIRE(r.pnl == Catch::Approx(-170.0 * 4.0));
}

TEST_CASE("Brain replay: the lot multiplier and the number of levels are what it says") {
    BrainConfig cfg;
    cfg.stepAtr = 0.0;
    cfg.maxPositions = 2;
    cfg.lotMultiplier = 1.5;
    std::vector<Bar> bars{bar(0, 100, 100, 100)};
    for (int i = 1; i <= 6; ++i) bars.push_back(i % 2 ? bar(i, 102.1, 100, 102) : bar(i, 100, 97.9, 98));
    const ReplayCycle r = replayCycle(bars, {}, 0, 4.0, 0.0, cfg);
    REQUIRE(r.locked);
    REQUIRE(r.fills == 2);
    // Buy 1 at 102, sell 1.5 at 98; the lock (a buy) at 102: the buy flat, the sell -1.5 x 4.
    REQUIRE(r.pnl == Catch::Approx(-6.0));
}

TEST_CASE("Brain replay: every 3 trades the lines move out to a wider step") {
    BrainConfig cfg;  // 3 per step, + 2 x ATR
    std::vector<Bar> bars{bar(0, 100, 100, 100)};
    // Lines 102 / 98: buy, sell, buy -- then a step of 4 + 2 x 1 = 6: 103 / 97.
    bars.push_back(bar(1, 102.1, 100, 102));
    bars.push_back(bar(2, 100, 97.9, 98));
    bars.push_back(bar(3, 102.1, 100, 102));
    bars.push_back(bar(4, 100, 97.5, 97.6));     // under 98 but not 97: nothing
    bars.push_back(bar(5, 100, 96.9, 97));       // the 4th trade, at the new 97
    const ReplayCycle r = replayCycle(bars, {}, 0, 4.0, 1.0, cfg);
    REQUIRE(r.fills == 4);
    // Buys 1 + 4 at 102, sells 2 at 98 and 8 at 97; at 97 the buys are 5 x 5 wrong, the 98 sell 2 x 1 right.
    REQUIRE(r.pnl == Catch::Approx(-25.0 + 2.0));
}

TEST_CASE("Brain decisions: an unknown or losing situation is no trade; a good one gets its gap") {
    BrainConfig cfg;
    MarketBrain brain;
    Situation s;
    s.atr = 2.0;
    s.known = true;
    REQUIRE_FALSE(decideWithBrain(brain, s, cfg).trade);  // not trained

    brain.trained = true;
    brain.gapAtrChoices = cfg.gapAtrChoices;
    brain.cells.assign(81, BrainCell{});
    const BrainDecision no = decideWithBrain(brain, s, cfg);
    REQUIRE_FALSE(no.trade);
    REQUIRE_THAT(no.why, Catch::Matchers::ContainsSubstring("no gap worth trading"));

    brain.cells[0] = BrainCell{50, 1, 1.5};  // 3 x ATR
    const BrainDecision yes = decideWithBrain(brain, s, cfg);
    REQUIRE(yes.trade);
    REQUIRE(yes.gap == Catch::Approx(6.0));

    s.known = false;
    REQUIRE_FALSE(decideWithBrain(brain, s, cfg).trade);
}

TEST_CASE("Brain training: too few bars teaches nothing; nothing is adopted without passing the test") {
    BrainConfig cfg;
    const std::vector<Bar> few = sideways(300);
    const BrainTraining none = trainBrain(few, {}, MarketBrain{}, cfg);
    REQUIRE_FALSE(none.better);
    REQUIRE_FALSE(none.brain.trained);

    const std::vector<Bar> bars = sideways(6000);
    const BrainTraining t = trainBrain(bars, {}, MarketBrain{}, cfg);
    REQUIRE(t.tested.trained);
    REQUIRE(t.report.find("fixed 3 x ATR") != std::string::npos);
    REQUIRE(t.brain.trained == t.better);
}

TEST_CASE("Brain learning: bars after the end it's given change nothing") {
    BrainConfig cfg;
    std::vector<Bar> bars = sideways(6000);
    const MarketBrain before = learnBrain(bars, {}, 4000, cfg);
    for (std::size_t i = 4000; i < bars.size(); ++i) {  // a crash, after that point
        bars[i].high += 500.0;
        bars[i].low -= 500.0;
        bars[i].close -= 200.0;
    }
    const MarketBrain after = learnBrain(bars, {}, 4000, cfg);
    REQUIRE(before.cells.size() == after.cells.size());
    for (std::size_t i = 0; i < before.cells.size(); ++i) {
        REQUIRE(before.cells[i].bestChoice == after.cells[i].bestChoice);
        REQUIRE(before.cells[i].meanPnl == Catch::Approx(after.cells[i].meanPnl));
    }
}

TEST_CASE("Regime range: the default gap limits are $2 and $15, read from the last minutes") {
    RegimeConfig cfg;
    REQUIRE(cfg.minGap == Catch::Approx(2.0));
    REQUIRE(cfg.maxGap == Catch::Approx(15.0));
    std::deque<Bar> minutes;
    double prev = 4000.0;
    for (int i = 0; i < 240; ++i) {
        const double c = 4000.0 + 0.8 * std::sin(i * 0.9);
        minutes.push_back(Bar{i * 60'000LL, prev, std::max(prev, c) + 0.1, std::min(prev, c) - 0.1, c, 10.0});
        prev = c;
    }
    const RegimeRead read = readRegime(minutes, cfg);
    const RegimeRange r = computeRegimeRange(read, minutes.back().close, {}, 0.0, 0.0, cfg);
    REQUIRE(r.buyStop - r.sellStop >= 2.0 - 1e-9);
    REQUIRE(r.buyStop - r.sellStop <= 15.0 + 1e-9);
    // The brain's gap, when it gives one, is the gap.
    const RegimeRange chosen = computeRegimeRange(read, minutes.back().close, {}, 0.0, 0.0, cfg, 9.0);
    REQUIRE(chosen.buyStop - chosen.sellStop == Catch::Approx(9.0));
}
