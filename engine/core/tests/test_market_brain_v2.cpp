#include <cmath>
#include <deque>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "muuk/core/market_brain_v2.hpp"

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

// A cleaner range: a wide slow swing, little fast noise, so cycles reach
// their target instead of whipsawing to the lock.
std::vector<Bar> ranging(int count, double swing = 6.0, double noise = 0.2) {
    std::vector<Bar> out;
    double prev = 4000.0;
    for (int i = 0; i < count; ++i) {
        const double c = 4000.0 + swing * std::sin(i * 0.05) + noise * std::sin(i * 1.3);
        out.push_back(Bar{i * 60'000LL, prev, std::max(prev, c) + 0.2, std::min(prev, c) - 0.2, c, 20.0});
        prev = c;
    }
    return out;
}

BrainV2 trainedBrain() {
    BrainV2 brain;
    brain.trained = true;
    brain.volLow = 0.5;
    brain.volHigh = 1.0;
    brain.gapAtrChoices = {2.0, 3.0, 4.0, 6.0, 8.0};
    brain.cells.assign(81, BrainV2Cell{});
    BrainV2Cell& cell = brain.cells[0];
    cell.samples = 50;
    cell.best = BrainV2Policy{1, 2.0, 8, 1.0};  // 3 x ATR, x2, 8 levels — the dangerous v1 policy
    cell.meanPnl = 5.0;
    cell.sumPnl = 250.0;
    cell.sumSquares = 1300.0;
    cell.locks = 0;
    cell.lockRate = 0.0;
    cell.score = 4.0;
    return brain;
}

SituationV2 situation() {
    SituationV2 s;
    s.base.atr = 2.0;
    s.base.known = true;
    s.known = true;
    return s;
}

}  // namespace

TEST_CASE("Brain v2 lock cost: the v1 x2/8 policy really does cost $680 at a $4 gap") {
    REQUIRE(estimatedLockCost(4.0, BrainV2Policy{-1, 2.0, 8, 1.0}) == Catch::Approx(680.0));
    REQUIRE(estimatedLockCost(4.0, BrainV2Policy{-1, 1.0, 3, 1.0}) == Catch::Approx(8.0));
    REQUIRE(estimatedLockCost(6.0, BrainV2Policy{-1, 1.0, 3, 1.0}) == Catch::Approx(12.0));
    REQUIRE(estimatedLockCost(6.0, BrainV2Policy{-1, 1.5, 6, 1.0}) == Catch::Approx(74.8125));
}

TEST_CASE("Brain v2 replay: slippage and commission are charged on every fill") {
    const std::vector<Bar> bars{bar(0, 100, 100, 100), bar(1, 102.5, 100, 102.2), bar(2, 100.5, 99.5, 100)};
    const BrainV2Policy policy{-1, 1.0, 8, 1.0};
    BrainV2Config free;
    free.slippage = 0.0;
    REQUIRE(replayCycleV2(bars, {}, 0, 4.0, 1.0, policy, free).pnl == Catch::Approx(-2.0));
    BrainV2Config slipped;
    slipped.slippage = 0.10;
    REQUIRE(replayCycleV2(bars, {}, 0, 4.0, 1.0, policy, slipped).pnl == Catch::Approx(-2.1));
    BrainV2Config charged = slipped;
    charged.commission = 0.05;
    REQUIRE(replayCycleV2(bars, {}, 0, 4.0, 1.0, policy, charged).pnl == Catch::Approx(-2.15));
}

TEST_CASE("Brain v2 caps: the brain can propose x2/8, the caps downgrade it, and a tiny cap refuses it") {
    BrainV2Config cfg;
    const BrainV2 brain = trainedBrain();
    const SituationV2 s = situation();
    SECTION("default caps: 8 levels become 5, the lot multiplier stays") {
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.policy.maxLevels == 5);
        REQUIRE(d.policy.lotMultiplier == Catch::Approx(2.0));
        REQUIRE(d.estimatedLockCost <= cfg.caps.maxLockCost);
        REQUIRE_THAT(d.why, Catch::Matchers::ContainsSubstring("downgraded"));
    }
    SECTION("a $5 lock cap refuses even the smallest policy") {
        cfg.caps.maxLockCost = 5.0;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE_FALSE(d.trade);
        REQUIRE_THAT(d.why, Catch::Matchers::ContainsSubstring("lock cost"));
    }
    SECTION("daily loss limit and spread cap refuse, whatever the brain says") {
        BrainAccountState broke;
        broke.dailyPnl = -100.0;
        cfg.caps.maxDailyLoss = 50.0;
        REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, broke).trade);
        BrainAccountState wide;
        wide.spread = 1.0;
        cfg.caps.maxDailyLoss = 0.0;
        cfg.caps.maxSpread = 0.5;
        REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, wide).trade);
    }
    SECTION("caps never raise a policy, only lower it") {
        cfg.caps.maxLockCost = 10000.0;
        cfg.caps.maxSingleLot = 1000.0;
        cfg.caps.maxLevelsHard = 8;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.policy.maxLevels == 8);
        REQUIRE(d.policy.lotMultiplier == Catch::Approx(2.0));
    }
}

TEST_CASE("Brain v2 gates: strong trend, a lost last basket and a wide spread all say no") {
    BrainV2Config cfg;
    cfg.caps.maxLockCost = 10000.0;
    cfg.caps.maxLevelsHard = 8;
    cfg.caps.maxSingleLot = 1000.0;
    const BrainV2 brain = trainedBrain();
    SituationV2 s = situation();
    REQUIRE(decideWithBrainV2(brain, s, cfg, {}).trade);

    s.base.trend = 2;
    s.trendDir = 1;
    REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, {}).trade);

    s = situation();
    s.recentOutcome = -1;
    BrainV2 weak = brain;
    weak.cells[0].meanPnl = 0.5;
    weak.cells[0].score = 0.1;
    REQUIRE_FALSE(decideWithBrainV2(weak, s, cfg, {}).trade);
    REQUIRE(decideWithBrainV2(brain, s, cfg, {}).trade);  // mean 5 clears the gate

    s = situation();
    s.spreadPct = 3.0;
    REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, {}).trade);
}

TEST_CASE("Brain v2 online learning: one bad basket can veto a cell, one small one cannot flip it") {
    BrainV2Config cfg;
    BrainV2 brain = trainedBrain();
    brain.cells[0].best = BrainV2Policy{1, 1.0, 3, 1.0};
    brain.cells[0].meanPnl = 2.0;
    brain.cells[0].sumPnl = 100.0;
    brain.cells[0].sumSquares = 210.0;
    brain.cells[0].score = 1.9;
    const SituationV2 s = situation();
    const BrainV2Policy used{1, 1.0, 3, 1.0};

    SECTION("a locked basket losing $500 vetoes the cell") {
        observeBasketOutcome(brain, s, used, -500.0, true, cfg);
        REQUIRE(brain.cells[0].best.gapChoice == -1);
        REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, {}).trade);
    }
    SECTION("a $10 loss on a 50-sample cell leaves it trading") {
        observeBasketOutcome(brain, s, used, -10.0, false, cfg);
        REQUIRE(brain.cells[0].best.gapChoice == 1);
        REQUIRE(brain.cells[0].meanPnl == Catch::Approx(90.0 / 51.0));
    }
    SECTION("an outcome for a policy the cell does not use changes nothing") {
        observeBasketOutcome(brain, s, BrainV2Policy{2, 1.0, 3, 1.0}, -500.0, true, cfg);
        REQUIRE(brain.cells[0].best.gapChoice == 1);
        REQUIRE(brain.cells[0].meanPnl == Catch::Approx(2.0));
    }
}

TEST_CASE("Brain v2 file: a brain survives a save and a load") {
    const BrainV2 brain = trainedBrain();
    const std::string text = serializeBrainV2(brain);
    const std::optional<BrainV2> back = parseBrainV2(text);
    REQUIRE(back.has_value());
    REQUIRE(back->trained);
    REQUIRE(back->volLow == Catch::Approx(0.5));
    REQUIRE(back->gapAtrChoices.size() == 5);
    REQUIRE(back->cells[0].best.gapChoice == 1);
    REQUIRE(back->cells[0].best.maxLevels == 8);
    REQUIRE(back->cells[0].meanPnl == Catch::Approx(5.0));
    REQUIRE_FALSE(parseBrainV2("not a brain").has_value());
    REQUIRE_FALSE(parseBrainV2("muuk-brain-v2 2\n").has_value());
}

TEST_CASE("Brain v2 learning: on ranging bars it learns a policy, never x2/8, and the test decides adoption") {
    BrainV2Config cfg;
    cfg.base.periods = 3;
    const std::vector<Bar> bars = ranging(6000);
    const BrainV2 brain = learnBrainV2(bars, {}, 6000, cfg);
    REQUIRE(brain.trained);
    REQUIRE(brain.cells.size() == 81);
    int tradable = 0;
    for (const BrainV2Cell& c : brain.cells) {
        if (c.best.gapChoice < 0) continue;
        ++tradable;
        REQUIRE(c.best.lotMultiplier <= 2.0);
        REQUIRE(c.best.maxLevels <= 6);
        REQUIRE(c.score > 0.0);
    }
    REQUIRE(tradable >= 1);

    // On the whipsaw-heavy series v2 may learn nothing tradable, but it
    // must never adopt a dangerous policy there.
    const BrainV2 whipsaw = learnBrainV2(sideways(6000), {}, 6000, cfg);
    REQUIRE(whipsaw.trained);
    for (const BrainV2Cell& c : whipsaw.cells) {
        if (c.best.gapChoice < 0) continue;
        REQUIRE(c.best.lotMultiplier <= 2.0);
        REQUIRE(c.best.maxLevels <= 6);
        REQUIRE(c.lockRate <= 0.05);
    }

    const BrainV2Training t = trainBrainV2(bars, {}, BrainV2{}, cfg);
    REQUIRE(t.tested.trained);
    REQUIRE(t.report.find("v2 brain") != std::string::npos);
    REQUIRE(t.report.find("gates:") != std::string::npos);
    REQUIRE(t.brain.trained == t.better);
}
