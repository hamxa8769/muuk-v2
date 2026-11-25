#include <algorithm>
#include <cmath>
#include <deque>
#include <sstream>
#include <string>
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

// A steady fall with small pullbacks: the buy line is never hit first,
// so a Sell-first replay is exactly what a Both replay is -- a tie the
// 0.05 margin keeps as Both.
std::vector<Bar> falling(int count) {
    std::vector<Bar> out;
    double prev = 4000.0;
    for (int i = 0; i < count; ++i) {
        const double c = 4000.0 - 0.05 * i + 0.3 * std::sin(i * 0.3);
        out.push_back(Bar{i * 60'000LL, prev, std::max(prev, c) + 0.2, std::min(prev, c) - 0.2, c, 20.0});
        prev = c;
    }
    return out;
}

// Strip the trailing firstSide from every cell line: the file a
// pre-Step-3 build wrote (15 fields per cell line, not 16).
std::string stripFirstSide(const std::string& text) {
    std::istringstream in(text);
    std::string line, out;
    bool first = true;
    while (std::getline(in, line)) {
        if (!first) out += "\n";
        first = false;
        std::istringstream fields(line);
        std::vector<std::string> tokens;
        std::string tok;
        while (fields >> tok) tokens.push_back(tok);
        if (tokens.size() == 15) tokens.pop_back();  // a cell line
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (i > 0) out += " ";
            out += tokens[i];
        }
    }
    return out;
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

TEST_CASE("Brain v2 caps: with a stop loss the lock cap binds at the stop, not the no-stop lock cost") {
    // The fixture brain proposes x2/8 at a $6 gap: no-stop lock cost
    // $1020, so the $150 cap cuts it to 5 levels (lock cost $126).
    BrainV2Config cfg;
    cfg.caps.maxSingleLot = 1000.0;
    cfg.caps.maxLevelsHard = 8;
    const BrainV2 brain = trainedBrain();
    const SituationV2 s = situation();
    SECTION("without a stop the $150 lock cap cuts x2/8 to 5 levels") {
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.policy.maxLevels == 5);
        REQUIRE(d.estimatedLockCost == Catch::Approx(126.0));
    }
    SECTION("with a $50 stop the same policy keeps all 8 levels: the worst case is the stop") {
        cfg.stopLoss = 50.0;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.policy.maxLevels == 8);
        REQUIRE(d.policy.lotMultiplier == Catch::Approx(2.0));
        REQUIRE(d.estimatedLockCost == Catch::Approx(50.0));
    }
    SECTION("a stop wider than the lock cost changes nothing") {
        cfg.stopLoss = 2000.0;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.policy.maxLevels == 5);
        REQUIRE(d.estimatedLockCost == Catch::Approx(126.0));
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
    brain.cells[0].samples = 100;
    brain.cells[0].best = BrainV2Policy{1, 1.0, 3, 1.0};
    brain.cells[0].meanPnl = 2.0;
    brain.cells[0].sumPnl = 200.0;
    brain.cells[0].sumSquares = 420.0;
    brain.cells[0].score = 1.9;
    const SituationV2 s = situation();
    const BrainV2Policy used{1, 1.0, 3, 1.0};

    SECTION("a locked basket losing $500 vetoes the cell") {
        observeBasketOutcome(brain, s, used, -500.0, true, cfg);
        REQUIRE(brain.cells[0].best.gapChoice == -1);
        REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, {}).trade);
    }
    SECTION("a $10 loss on a 100-sample cell leaves it trading") {
        // (100 samples: the shrunk lock rate of a clean cell is 1/111 = 0.9%,
        // below the 0.015 filter; a 50-sample cell would shrink to 1.64%
        // and be vetoed even with zero locks.)
        observeBasketOutcome(brain, s, used, -10.0, false, cfg);
        REQUIRE(brain.cells[0].best.gapChoice == 1);
        REQUIRE(brain.cells[0].meanPnl == Catch::Approx(190.0 / 101.0));
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

TEST_CASE("Brain v2 file: the trained shift survives a save and a load, and an old file loads without one") {
    BrainV2 brain = trainedBrain();
    brain.shiftAfterFills = 3;
    brain.stepAtr = 2.0;
    const std::string text = serializeBrainV2(brain);
    const std::optional<BrainV2> back = parseBrainV2(text);
    REQUIRE(back.has_value());
    REQUIRE(back->shiftAfterFills == 3);
    REQUIRE(back->stepAtr == Catch::Approx(2.0));
    REQUIRE(back->trained);

    // An old file ends its header line after forced -- no shift fields.
    // It must still parse, with shift 0: the runner then falls back to
    // the config shift instead of refusing the brain.
    std::string oldText = text;
    const std::string shiftFields = " shiftAfterFills 3 stepAtr 2";
    const std::size_t at = oldText.find(shiftFields);
    REQUIRE(at != std::string::npos);
    oldText.erase(at, shiftFields.size());
    const std::optional<BrainV2> oldBack = parseBrainV2(oldText);
    REQUIRE(oldBack.has_value());
    REQUIRE(oldBack->trained);
    REQUIRE(oldBack->shiftAfterFills == 0);
    REQUIRE(oldBack->stepAtr == Catch::Approx(0.0));
    REQUIRE(oldBack->cells[0].best.gapChoice == 1);
}

TEST_CASE("Brain v2 file: the trained erMode and stop loss survive a save and a load, and an old file loads without them") {
    BrainV2 brain = trainedBrain();
    brain.erMode = 2;
    brain.stopLoss = 50.0;
    brain.shiftAfterFills = 3;
    brain.stepAtr = 2.0;
    const std::string text = serializeBrainV2(brain);
    const std::optional<BrainV2> back = parseBrainV2(text);
    REQUIRE(back.has_value());
    REQUIRE(back->erMode == 2);
    REQUIRE(back->stopLoss == Catch::Approx(50.0));
    REQUIRE(back->shiftAfterFills == 3);
    REQUIRE(back->trained);

    // An old file ends its header line after stepAtr -- no erMode/stopLoss.
    // It must still parse, with erMode -1 (not recorded): the runner then
    // falls back to the panel's erMode instead of refusing the brain.
    std::string oldText = text;
    const std::string newFields = " erMode 2 stopLoss 50";
    const std::size_t at = oldText.find(newFields);
    REQUIRE(at != std::string::npos);
    oldText.erase(at, newFields.size());
    const std::optional<BrainV2> oldBack = parseBrainV2(oldText);
    REQUIRE(oldBack.has_value());
    REQUIRE(oldBack->trained);
    REQUIRE(oldBack->erMode == -1);
    REQUIRE(oldBack->stopLoss == Catch::Approx(0.0));
    REQUIRE(oldBack->cells[0].best.gapChoice == 1);
}

TEST_CASE("Brain v2 learning: the brain records the shift it trained with") {
    BrainV2Config cfg;
    cfg.base.periods = 3;
    cfg.base.shiftAfterFills = 4;
    cfg.base.stepAtr = 1.0;
    const BrainV2 brain = learnBrainV2(ranging(12000), {}, 12000, cfg);
    REQUIRE(brain.trained);
    REQUIRE(brain.shiftAfterFills == 4);
    REQUIRE(brain.stepAtr == Catch::Approx(1.0));
}

TEST_CASE("Brain v2 learning: the brain records the erMode and stop loss it trained with") {
    BrainV2Config cfg;
    cfg.base.periods = 3;
    cfg.erMode = 2;
    cfg.stopLoss = 50.0;
    const BrainV2 brain = learnBrainV2(ranging(12000), {}, 12000, cfg);
    REQUIRE(brain.trained);
    REQUIRE(brain.erMode == 2);
    REQUIRE(brain.stopLoss == Catch::Approx(50.0));
}

TEST_CASE("Shift search: the best gates-passing combo wins, held-out locks outrank the total") {
    const ShiftSearchCandidate passingFewLocks{2, 1.0, true, 100.0, 5, 50.0, 1, 6};
    const ShiftSearchCandidate passingMoreTotal{3, 2.0, true, 400.0, 2, 40.0, 3, 7};
    const ShiftSearchCandidate failingHuge{4, 2.0, false, 900.0, 0, 500.0, 0, 9};
    // Fewest held-out locks wins, even though the other combo made more.
    REQUIRE(pickBestShiftCombo({passingMoreTotal, passingFewLocks})->shiftAfterFills == 2);
    // A gates-failing combo never wins, however good its numbers look.
    REQUIRE(pickBestShiftCombo({failingHuge, passingMoreTotal})->shiftAfterFills == 3);
    // Held-out PnL breaks a tie on held-out locks.
    const ShiftSearchCandidate passingBetterHeldOut{4, 1.0, true, 100.0, 5, 80.0, 1, 6};
    REQUIRE(pickBestShiftCombo({passingFewLocks, passingBetterHeldOut})->shiftAfterFills == 4);
    // Nothing passed: no pick, so the shipped shift stays.
    REQUIRE_FALSE(pickBestShiftCombo({failingHuge}).has_value());
    REQUIRE_FALSE(pickBestShiftCombo({}).has_value());
}

TEST_CASE("Brain v2 learning: on ranging bars it learns a policy, never x2/8, and the test decides adoption") {
    BrainV2Config cfg;
    cfg.base.periods = 3;
    const std::vector<Bar> bars = ranging(12000);
    const BrainV2 brain = learnBrainV2(bars, {}, 12000, cfg);
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

TEST_CASE("Kaufman ER: a straight rise is ~1, a whipsaw is ~0, too few bars is -1") {
    std::deque<Bar> rising;
    for (int i = 0; i < 21; ++i) rising.push_back(bar(i, 4000.0 + i, 4000.0 + i, 4000.0 + i));
    REQUIRE(kaufmanEr(rising, 20) == Catch::Approx(1.0));
    std::deque<Bar> whipsaw;
    for (int i = 0; i < 21; ++i) whipsaw.push_back(bar(i, 4010.0, 4000.0, i % 2 == 0 ? 4000.0 : 4010.0));
    REQUIRE(kaufmanEr(whipsaw, 20) == Catch::Approx(0.0));
    std::deque<Bar> few;
    for (int i = 0; i < 10; ++i) few.push_back(bar(i, 4000.0, 4000.0, 4000.0));
    REQUIRE(kaufmanEr(few, 20) == -1.0);
    std::deque<Bar> flat;
    for (int i = 0; i < 21; ++i) flat.push_back(bar(i, 4000.0, 4000.0, 4000.0));
    REQUIRE(kaufmanEr(flat, 20) == -1.0);
}

TEST_CASE("ATR percentile: rising volatility ranks high, falling ranks low") {
    auto series = [](int count, double startRange, double step) {
        std::deque<Bar> out;
        for (int i = 0; i < count; ++i) {
            const double half = (startRange + step * i) / 2.0;
            out.push_back(Bar{i * 60'000LL, 4000.0, 4000.0 + half, 4000.0 - half, 4000.0, 10.0});
        }
        return out;
    };
    // Range grows 1 -> ~11: the last ATR is the highest of the window.
    REQUIRE(atrPercentile(series(100, 1.0, 0.1), 14) > 90.0);
    // Range shrinks 11 -> 1: the last ATR is the lowest of the window.
    REQUIRE(atrPercentile(series(100, 11.0, -0.1), 14) < 10.0);
    // Too few bars: unknown.
    REQUIRE(atrPercentile(series(30, 1.0, 0.1), 14) == -1.0);
}

TEST_CASE("ER gate: chop blocks in mode 1, a clean trend overrides the trend gate and the breaker in mode 2") {
    BrainV2Config cfg;
    cfg.caps.maxLockCost = 10000.0;
    cfg.caps.maxLevelsHard = 8;
    cfg.caps.maxSingleLot = 1000.0;
    BrainV2 brain = trainedBrain();
    // The trend is part of the cell index (vol*27 + trend*9 + chop*3 +
    // session), so a strong-trend situation lands in a different cell than
    // the no-trend one. Make that cell tradable too, with the same policy,
    // so the trend gate -- not the cell -- decides the outcome.
    BrainV2Cell& trendCell = brain.cells[0 * 27 + 2 * 9 + 0 * 3 + 0];
    trendCell.samples = 50;
    trendCell.best = BrainV2Policy{1, 2.0, 8, 1.0};
    trendCell.meanPnl = 5.0;
    trendCell.sumPnl = 250.0;
    trendCell.sumSquares = 1300.0;
    trendCell.locks = 0;
    trendCell.lockRate = 0.0;
    trendCell.score = 4.0;

    SECTION("mode 0 is the shipped behaviour: ER changes nothing") {
        cfg.erMode = 0;
        SituationV2 s = situation();
        s.er = 0.8;
        s.base.trend = 2;
        s.trendDir = 1;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE_FALSE(d.trade);
        REQUIRE(d.blockedBy == BrainBlockReason::StrongTrend);
    }
    SECTION("mode 1 blocks a choppy cycle") {
        cfg.erMode = 1;
        SituationV2 s = situation();
        s.er = 0.1;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE_FALSE(d.trade);
        REQUIRE(d.blockedBy == BrainBlockReason::ErChop);
        s.er = 0.8;
        REQUIRE(decideWithBrainV2(brain, s, cfg, {}).trade);
    }
    SECTION("mode 2 lets a clean trend through the strong-trend gate") {
        cfg.erMode = 2;
        SituationV2 s = situation();
        s.base.trend = 2;
        s.trendDir = 1;
        s.er = 0.8;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.blockedBy == BrainBlockReason::None);
        REQUIRE_THAT(d.why, Catch::Matchers::ContainsSubstring("overrode"));
        s.er = 0.1;
        REQUIRE_FALSE(decideWithBrainV2(brain, s, cfg, {}).trade);
    }
    SECTION("mode 2 lets a clean trend through the ATR breaker") {
        cfg.erMode = 2;
        SituationV2 s = situation();
        s.base.atr = 3.0;  // over 2.0x volHigh 1.0
        s.er = 0.8;
        REQUIRE(decideWithBrainV2(brain, s, cfg, {}).trade);
        cfg.erMode = 0;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE_FALSE(d.trade);
        REQUIRE(d.blockedBy == BrainBlockReason::AtrBreaker);
    }
    SECTION("mode 3 replaces the fixed breaker with the ATR percentile") {
        cfg.erMode = 3;
        SituationV2 s = situation();
        s.base.atr = 3.0;  // fixed breaker would fire, but mode 3 replaces it
        s.atrPct = 50.0;   // under the 90 block
        s.er = -1.0;
        REQUIRE(decideWithBrainV2(brain, s, cfg, {}).trade);
        s.atrPct = 95.0;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE_FALSE(d.trade);
        REQUIRE(d.blockedBy == BrainBlockReason::AtrBreaker);
        s.er = 0.8;  // clean trend overrides the percentile block too
        REQUIRE(decideWithBrainV2(brain, s, cfg, {}).trade);
    }
}

TEST_CASE("Brain v2 first side: resolveFirstSide follows the trend only for trend-first") {
    REQUIRE(resolveFirstSide(2, 1) == 1);
    REQUIRE(resolveFirstSide(2, -1) == -1);
    REQUIRE(resolveFirstSide(2, 0) == 0);
    REQUIRE(resolveFirstSide(1, -1) == 1);
    REQUIRE(resolveFirstSide(-1, 1) == -1);
    REQUIRE(resolveFirstSide(0, 1) == 0);
    REQUIRE(resolveFirstSide(7, 1) == 0);  // unknown value: both stops
}

TEST_CASE("Brain v2 replay: the first side arms one stop only") {
    // Gap 4.00 around 4000: the Buy Stop at 4002 fills first (the price
    // rises just past it), then the crash fills the Sell Stop at 3998 and
    // runs to 3996. Both takes both legs and ends -4.10; Sell-first waits
    // out the rise, sells the crash and books the +2.00 target at the
    // bottom; Buy-first is Both here (the buy fills first either way).
    BrainV2Config cfg;
    cfg.base.shiftAfterFills = 0;  // fixed lines: the shift is not what this tests
    const BrainV2Policy policy{2, 1.0, 3, 1.0};
    const std::vector<Bar> bars = {
        bar(0, 4000.0, 4000.0, 4000.0), bar(1, 4002.2, 3999.8, 4001.0), bar(2, 4001.0, 3997.8, 3998.0),
        bar(3, 3998.0, 3995.8, 3996.0), bar(4, 3996.2, 3995.8, 3996.0), bar(5, 3996.2, 3995.8, 3996.0),
        bar(6, 3996.2, 3995.8, 3996.0), bar(7, 3996.2, 3995.8, 3996.0), bar(8, 3996.2, 3995.8, 3996.0),
        bar(9, 3996.2, 3995.8, 3996.0), bar(10, 3996.2, 3995.8, 3996.0),
    };
    const ReplayCycleV2 both = replayCycleV2(bars, {}, 0, 4.0, 1.0, policy, cfg, 0);
    REQUIRE(both.fills == 2);
    REQUIRE(both.pnl == Catch::Approx(-4.10));
    REQUIRE_FALSE(both.locked);
    const ReplayCycleV2 sell = replayCycleV2(bars, {}, 0, 4.0, 1.0, policy, cfg, -1);
    REQUIRE(sell.fills == 1);
    REQUIRE(sell.pnl == Catch::Approx(2.0));
    REQUIRE(sell.endBar == 3);
    const ReplayCycleV2 buy = replayCycleV2(bars, {}, 0, 4.0, 1.0, policy, cfg, 1);
    REQUIRE(buy.fills == 2);
    REQUIRE(buy.pnl == Catch::Approx(-4.10));
}

TEST_CASE("Brain v2 decision: the first side follows the cell, and the trend side follows the trend") {
    BrainV2Config cfg;
    cfg.caps.maxLockCost = 10000.0;
    cfg.caps.maxLevelsHard = 8;
    cfg.caps.maxSingleLot = 1000.0;
    BrainV2 brain = trainedBrain();
    BrainV2Cell& cell = brain.cells[0];
    SituationV2 s = situation();

    SECTION("a Buy-first cell opens Buy first, whatever the trend") {
        cell.firstSide = 1;
        s.trendDir = -1;  // even against the trend: Buy-first is a fixed side
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.firstSide == 1);
        REQUIRE_THAT(d.why, Catch::Matchers::ContainsSubstring("first Buy"));
    }
    SECTION("a Sell-first cell opens Sell first") {
        cell.firstSide = -1;
        s.trendDir = 1;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.firstSide == -1);
        REQUIRE_THAT(d.why, Catch::Matchers::ContainsSubstring("first Sell"));
    }
    SECTION("a trend-first cell follows the cycle's trend direction") {
        cell.firstSide = 2;
        s.trendDir = 1;
        REQUIRE(decideWithBrainV2(brain, s, cfg, {}).firstSide == 1);
        s.trendDir = -1;
        REQUIRE(decideWithBrainV2(brain, s, cfg, {}).firstSide == -1);
        s.trendDir = 0;  // no clear direction: both stops, as before
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.firstSide == 0);
    }
    SECTION("an undecided cell arms both stops") {
        cell.firstSide = 0;
        const BrainV2Decision d = decideWithBrainV2(brain, s, cfg, {});
        REQUIRE(d.trade);
        REQUIRE(d.firstSide == 0);
        REQUIRE_THAT(d.why, Catch::Matchers::ContainsSubstring("first both"));
    }
}

TEST_CASE("Brain v2 file: the first side survives a save and a load, and an old file loads as Both") {
    BrainV2 brain = trainedBrain();
    brain.cells[0].firstSide = -1;
    brain.cells[1].firstSide = 2;
    const std::string text = serializeBrainV2(brain);
    const std::optional<BrainV2> back = parseBrainV2(text);
    REQUIRE(back.has_value());
    REQUIRE(back->cells[0].firstSide == -1);
    REQUIRE(back->cells[1].firstSide == 2);
    REQUIRE(back->cells[2].firstSide == 0);

    // A file written before Step 3 ends each cell line after score: it
    // must still parse, with every cell on Both (the config first order
    // as the fallback).
    const std::string oldText = stripFirstSide(text);
    const std::optional<BrainV2> oldBack = parseBrainV2(oldText);
    REQUIRE(oldBack.has_value());
    REQUIRE(oldBack->trained);
    REQUIRE(oldBack->cells[0].firstSide == 0);
    REQUIRE(oldBack->cells[1].firstSide == 0);
    REQUIRE(oldBack->cells[0].best.gapChoice == 1);
}

TEST_CASE("Brain v2 learning: a cell whose history ties Sell-first with Both stays undecided") {
    BrainV2Config cfg;
    cfg.base.periods = 3;
    cfg.base.shiftAfterFills = 0;
    const BrainV2 brain = learnBrainV2(falling(12000), {}, 12000, cfg);
    REQUIRE(brain.trained);
    int tradable = 0;
    for (const BrainV2Cell& c : brain.cells) {
        if (c.best.gapChoice < 0) continue;
        ++tradable;
        REQUIRE(c.firstSide == 0);
    }
    REQUIRE(tradable >= 1);
}

TEST_CASE("Brain v2 learning: the first side it learns is always a valid side") {
    BrainV2Config cfg;
    cfg.base.periods = 3;
    const BrainV2 brain = learnBrainV2(ranging(12000), {}, 12000, cfg);
    REQUIRE(brain.trained);
    for (const BrainV2Cell& c : brain.cells) {
        REQUIRE(c.firstSide >= -1);
        REQUIRE(c.firstSide <= 2);
    }
}

TEST_CASE("Brain v2 replay: the live stop loss stops the basket out before the lock") {
    // Mid 100, gap 8 (lines 104 / 96), x2 lots, 4 levels, no shift.
    // Fills: buy 104 (lot 1), sell 96 (lot 2), buy 104 (lot 4), sell 96
    // (lot 8). Costs 0.05 x (1+2+4+8) = 0.75, so the net at price p is
    // 439.25 - 5p: on the bar of the fourth fill the high (100) puts the
    // worst net at -60.75, past the $50 stop.
    const std::vector<Bar> bars{bar(0, 100, 100, 100), bar(1, 104, 100, 104), bar(2, 104, 96, 96),
                                bar(3, 100, 96, 98), bar(4, 104, 98, 104)};
    const BrainV2Policy policy{-1, 2.0, 4, 1.0};
    BrainV2Config stopped;
    stopped.base.shiftAfterFills = 0;
    stopped.stopLoss = 50.0;
    const ReplayCycleV2 withStop = replayCycleV2(bars, {}, 0, 8.0, 1.0, policy, stopped);
    REQUIRE(withStop.pnl == Catch::Approx(-50.0));
    REQUIRE_FALSE(withStop.locked);
    REQUIRE(withStop.fills == 4);
    REQUIRE(withStop.endBar == 3);
    // Without the stop the same basket runs on: the next bar touches the
    // buy line at max levels and locks at -80.75.
    BrainV2Config noStop;
    noStop.base.shiftAfterFills = 0;
    const ReplayCycleV2 withoutStop = replayCycleV2(bars, {}, 0, 8.0, 1.0, policy, noStop);
    REQUIRE(withoutStop.pnl == Catch::Approx(-80.75));
    REQUIRE(withoutStop.locked);
    REQUIRE(withoutStop.fills == 4);
    REQUIRE(withoutStop.endBar == 4);
}

TEST_CASE("Brain v2 training: the progress callback reports the walk-forward and the held-out") {
    const std::vector<Bar> bars = ranging(6000);
    BrainV2Config cfg;
    std::vector<std::string> stages;
    int calls = 0;
    const BrainV2Training t = trainBrainV2(bars, {}, BrainV2{}, cfg,
        [&](const char* stage, int done, int total) {
            ++calls;
            stages.emplace_back(stage);
            REQUIRE(done >= 0);
            REQUIRE(done <= total);
            REQUIRE(total > 0);
        });
    REQUIRE(calls > 0);
    REQUIRE(std::find(stages.begin(), stages.end(), "walk-forward") != stages.end());
    REQUIRE(std::find(stages.begin(), stages.end(), "fixed") != stages.end());
    REQUIRE(std::find(stages.begin(), stages.end(), "held-out") != stages.end());
    (void)t;
}
