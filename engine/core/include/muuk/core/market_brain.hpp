#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "muuk/core/market_read.hpp"

namespace muuk::core {

// What the bot has learned from history: which gap worked in which kind of
// market, and in which ones nothing did. It learns by replaying the strategy
// over past one-minute bars (each decision sees only the bars before it), and
// a new brain counts only if it does better walking forward: period after
// period, learning only from the bars before each.
// It may choose only the gap and whether to trade: never lots, limits or code.
struct BrainConfig {
    std::vector<double> gapAtrChoices{2.0, 3.0, 4.0, 6.0, 8.0};  // gap = this x the one-minute ATR
    double minGap = 2.0;
    double maxGap = 15.0;
    int stepBars = 15;        // a decision point every this many bars
    int maxCycleBars = 240;   // a cycle still open after this long is closed at the price
    int maxPositions = 8;
    double lotMultiplier = 2.0;  // each level's lot x this (no rounding to the broker's lot step)
    int shiftAfterFills = 3;  // trades per step
    double stepAtr = 2.0;     // each step's gap: the last + this x ATR, at most maxGap
    int minSamples = 20;      // fewer in a situation: not known, so no trade
    double firstLearnShare = 0.3;  // walking forward: the first period starts after this share of the bars
    int periods = 11;              // and the rest is cut into this many
};

// A kind of market. Each from the bars before the decision only.
struct Situation {
    int volatility = 0;  // 0..2, against the terciles learned from history
    int trend = 0;       // 0..2, ADX(14) of 5-minute bars: under 20, under 30, more
    int chop = 0;        // 0..2, Choppiness(14) of 5-minute bars: under 45, under 60, more
    int session = 0;     // 0 Asia, 1 London, 2 New York (UTC hour of the bar)
    double atr = 0.0;    // one-minute ATR(14)
    bool known = false;  // enough bars
};

struct BrainCell {
    int samples = 0;
    int bestChoice = -1;   // index into gapAtrChoices; -1 = don't trade here
    double meanPnl = 0.0;  // of that choice, per 0.01 lot start ($1 per $1 move)
};

struct MarketBrain {
    bool trained = false;
    double volLow = 0.0;  // terciles of the one-minute ATR
    double volHigh = 0.0;
    std::vector<double> gapAtrChoices;
    std::vector<BrainCell> cells;  // 81: volatility x trend x chop x session
    int trainedBars = 0;
    int64_t trainedUntilMs = 0;
    bool forced = false;  // put to use by hand, without passing the test
};

struct BrainDecision {
    bool trade = false;
    double gap = 0.0;
    std::string why;
};

// How one set of rules did on bars, cycle after cycle, never overlapping.
struct ReplayResult {
    int cycles = 0;
    int wins = 0;
    int locks = 0;
    double totalPnl = 0.0;
    double worstCycle = 0.0;
    double maxDrawdown = 0.0;
};

// Walking forward over the periods. maxDrawdown: the deepest fall of the
// whole run, within the periods and across them.
struct WalkForwardResult {
    ReplayResult total;
    int periods = 0;
    int periodsUp = 0;
};

struct BrainTraining {
    MarketBrain brain;        // learned from all the bars, if the test passed
    MarketBrain tested;       // learned from the bars before the first period
    ReplayResult brainTest;   // walking forward, learning again before each period
    ReplayResult fixedTest;   // a fixed 3 x ATR on the same periods
    ReplayResult currentTest; // the brain in use on the same periods (none: no cycles)
    int periods = 0;
    int brainPeriodsUp = 0;
    bool better = false;      // profit, more than the fixed gap and the brain in use, no deeper drawdown than the fixed gap
    std::string report;
};

[[nodiscard]] Situation situationAt(const std::deque<Bar>& oneMinuteBars, const MarketBrain& brain);
[[nodiscard]] BrainDecision decideWithBrain(const MarketBrain& brain, const Situation& situation, const BrainConfig& config);
[[nodiscard]] double clampedGap(double gapAtr, double atr, const BrainConfig& config);

// One cycle from the close of bars[start]; pnl per 0.01 lot start.
struct ReplayCycle {
    double pnl = 0.0;
    int endBar = 0;
    int fills = 0;
    bool locked = false;
};
[[nodiscard]] ReplayCycle replayCycle(const std::vector<Bar>& bars, const std::vector<double>& spreads, int start, double gap,
                                      double atr, const BrainConfig& config);

// The table from bars[0 .. end): no test, nothing adopted.
[[nodiscard]] MarketBrain learnBrain(const std::vector<Bar>& bars, const std::vector<double>& spreads, int end,
                                     const BrainConfig& config);

// Cycle after cycle from bars[from], never overlapping: what the brain decides, or
// with fixedGapAtr > 0 that gap always.
[[nodiscard]] ReplayResult replayRules(const std::vector<Bar>& bars, const std::vector<double>& spreads, int from,
                                       const MarketBrain* brain, double fixedGapAtr, const BrainConfig& config);

// mode: a brain learned before each period (learnEachPeriod), the given brain, or (brain null, !learnEachPeriod) a fixed 3 x ATR.
[[nodiscard]] WalkForwardResult walkForward(const std::vector<Bar>& bars, const std::vector<double>& spreads, const MarketBrain* brain,
                                            bool learnEachPeriod, const BrainConfig& config);

// spreads: in price, one per bar (empty = none). current: the brain in use.
[[nodiscard]] BrainTraining trainBrain(const std::vector<Bar>& bars, const std::vector<double>& spreads,
                                       const MarketBrain& current, const BrainConfig& config);

}  // namespace muuk::core
