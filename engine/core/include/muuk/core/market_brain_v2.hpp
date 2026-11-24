#pragma once

// Brain v2: the super-upgrade of the v1 tabular brain (market_brain.hpp).
//
// What changes, in one paragraph: v1 learns only a gap per market state
// (81 cells, gap or no trade). v2 learns a whole *policy* per state — gap,
// lot multiplier, max levels and exit aggression — and it learns it with
// the real costs in the replay (spread + slippage + commission), with a
// lock-risk check that rejects or downgrades any policy whose history
// locks too often, and with hard risk caps the brain can never override.
// The state stays the v1 81-cell table (volatility x trend x chop x
// session) so every live decision is still explainable as
// "state X -> policy Y, N samples, mean +Z"; the extra features v2 sees
// (ATR trend, trend direction, RSI zone, Bollinger position, tick-volume,
// recent basket outcome, spread vs its median) act as *gates* on that
// decision, not as new cell dimensions — a full cross-product would need
// far more history than exists to fill. Online learning can only *veto*
// a cell (one bad live basket can turn it to no-trade); it can never
// invent a new gap, lot or level. Only a full re-train, gated by
// walk-forward, can change a policy. Demo-only until the gates pass:
// see BRAIN_UPGRADE.md.

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "muuk/core/market_brain.hpp"
#include "muuk/core/regime_range.hpp"

namespace muuk::core {

// One policy a cell can use. gapChoice -1 = no trade in this state.
struct BrainV2Policy {
    int gapChoice = -1;         // index into gapAtrChoices; -1 = don't trade here
    double lotMultiplier = 1.0; // each level's lot x this
    int maxLevels = 3;          // positions before the lock
    double exitFactor = 1.0;    // target = last lot x gap/2 x this (0.5 = exit earlier)
};

// Hard caps. The brain proposes; these dispose. A decision that would
// breach one is downgraded (fewer levels, smaller lots) or refused.
// All money figures are in replay units: $ per 0.01 lot start
// ($1 per $1 move), the same units as BrainTraining totals.
struct BrainRiskCaps {
    double maxDailyLoss = 0.0;  // 0 = off: stop new cycles once the day is this far down
    double maxDrawdown = 0.0;   // 0 = off: stop new cycles at this drawdown from the peak
    double maxLockCost = 150.0; // worst-case lock cost of the chosen policy must be <= this
    double maxLockCostPctOfBalance = 0.0;  // 0 = off: also cap lock cost at this share of balance
    double maxSingleLot = 16.0; // largest single leg, as a lot multiplier (1 = 0.01 lot)
    int maxLevelsHard = 6;      // never more levels than this, whatever the brain says
    double maxSpread = 0.0;     // 0 = off: no new cycle while the spread is above this, in price
};

struct BrainV2Config {
    BrainConfig base;  // v1 config: gaps, stepBars, maxCycleBars, minSamples, periods...
    double slippage = 0.05;    // $ per lot per fill, charged in the replay (was missing in v1)
    double commission = 0.0;   // $ per lot per fill, charged in the replay
    double maxLockRate = 0.015; // reject a policy whose (shrunk) lock rate is above this
    // (tightened from 0.05: the only lever that cut locks hard, 48->15 in the
    // 2026-11-20 run. It also cut the total to -$469 then, so it runs here
    // together with the volatility breaker, which skips the crash cycles.)
    bool noTradeInStrongTrend = true;  // ADX bucket 2 + a clear EMA direction: no new cycle
    double recentLossScore = 0.25;   // after a lost/locked basket, a cell needs score >= this
    double highVolumeScore = 0.25;     // in a tick-volume spike, a cell needs score >= this
    double maxSpreadMultiple = 2.0;    // current spread > this x its median: no trade (0 = off)
    double maxAtrVolMultiple = 2.0;    // current ATR > this x the training high-vol threshold: no trade (0 = off).
    // (1.5x was tried: locks 10 but total -$297 and held-out -$187 -- it
    // skips the cycles that were winning in the held-out. 2.0x stays.)
    // A circuit breaker for crashes: the Nov 2026 selloff's 1-min ATR runs 3-4x volHigh, and those cycles are
    // net-negative (37 of the baseline's 48 locks sit in the held-out crash).
    int onlineMaxSamples = 500;        // online tally: older outcomes fade past this
    BrainRiskCaps caps;
};

struct BrainV2Cell {
    int samples = 0;
    BrainV2Policy best;      // gapChoice -1 = no trade
    double meanPnl = 0.0;    // of the chosen policy, per 0.01 lot start
    double sumPnl = 0.0;     // running sum, for online updates
    double sumSquares = 0.0; // running sum of squares, for the standard error
    int locks = 0;
    double lockRate = 0.0;      // locks / samples
    double meanLockLoss = 0.0;  // average |pnl| of locked cycles, per 0.01 lot start
    double lockLossSum = 0.0;
    double score = 0.0;  // mean - 1 standard error
};

struct BrainV2 {
    bool trained = false;
    double volLow = 0.0;
    double volHigh = 0.0;
    std::vector<double> gapAtrChoices;
    std::vector<BrainV2Cell> cells;  // 81: volatility x trend x chop x session
    int trainedBars = 0;
    int64_t trainedUntilMs = 0;
    bool forced = false;  // put to use by hand, without passing the test
};

// The v1 situation, plus the context v2 gates on. recentOutcome and
// spreadPct are supplied by the caller (live: the last closed basket;
// replay: the last replayed cycle) — they are not cell dimensions.
struct SituationV2 {
    Situation base;       // v1: volatility, trend, chop, session, atr, known
    int atrTrend = 0;     // -1 falling, 0 flat, +1 rising (ATR now vs 3 hours ago)
    int trendDir = 0;     // -1 down, 0 none, +1 up (EMA8 vs EMA21, when the gap is clear)
    int rsiZone = 1;      // 0 oversold (<30), 1 mid, 2 overbought (>70)
    int bollingerPos = 1; // 0 below the lower band, 1 inside, 2 above the upper band
    int volume = 1;       // 0 quiet (<0.7x ticks), 1 normal, 2 spike (>1.5x ticks)
    int recentOutcome = 0;  // -1 last basket lost or locked, 0 none yet, +1 last basket won
    double spreadPct = 0.0; // current spread / median spread in the window (0 = unknown)
    bool known = false;
};

struct BrainV2Decision {
    bool trade = false;
    double gap = 0.0;
    BrainV2Policy policy;
    double lockRisk = 0.0;          // estimated P(lock) for this policy in this state
    double estimatedLockCost = 0.0; // worst-case lock cost, per 0.01 lot start
    std::string why;
};

// What the caps are checked against. Built by the caller each decision.
struct BrainAccountState {
    double dailyPnl = 0.0;  // negative = losing today, in replay units
    double drawdown = 0.0;  // from the equity peak, in replay units
    double spread = 0.0;    // current spread, in price
    double balance = 0.0;   // 0 = unknown; only used with maxLockCostPctOfBalance
};

struct ReplayCycleV2 {
    double pnl = 0.0;
    int endBar = 0;
    int fills = 0;
    bool locked = false;
};

struct BrainV2Training {
    BrainV2 brain;       // learned from all the bars, if the test passed
    BrainV2 tested;      // learned from the bars before the first period
    ReplayResult brainTest;  // v2 walking forward, learning again before each period
    ReplayResult fixedTest;  // fixed 3 x ATR on the same periods (v1 lot rule, v2 costs)
    ReplayResult v1Test;     // v1 brain walking forward on the same periods
    ReplayResult heldOutV2;  // v2 on the last 15% it never trained on
    ReplayResult heldOutFixed;
    ReplayResult heldOutV1;
    int periods = 0;
    int brainPeriodsUp = 0;
    bool better = false;      // v1 adoption rule: profit, better than fixed and v1, no deeper drawdown than fixed
    bool gatesPassed = false; // the stricter v2 gates: drawdown <= 50% of v1, locks <= 50% of v1, >=5 periods up
    std::string report;
};

[[nodiscard]] SituationV2 situationAtV2(const std::deque<Bar>& oneMinuteBars, const BrainV2& brain, int recentOutcome = 0,
                                         double spread = 0.0, double medianSpread = 0.0);
[[nodiscard]] BrainV2Decision decideWithBrainV2(const BrainV2& brain, const SituationV2& situation, const BrainV2Config& config,
                                                 const BrainAccountState& account = {});

// Worst-case lock cost of a policy at this gap: alternating fills to the
// lock, both starting sides, the worse one. Per 0.01 lot start.
[[nodiscard]] double estimatedLockCost(double gap, const BrainV2Policy& policy);

// A decision after the hard caps: downgraded (fewer levels / smaller lots)
// or refused. The brain's policy is never raised, only lowered.
[[nodiscard]] BrainV2Decision applyRiskCaps(BrainV2Decision decision, const BrainV2Config& config,
                                            const BrainAccountState& account);

[[nodiscard]] ReplayCycleV2 replayCycleV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int start, double gap,
                                           double atr, const BrainV2Policy& policy, const BrainV2Config& config);

[[nodiscard]] BrainV2 learnBrainV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int end,
                                    const BrainV2Config& config);

// Cycle after cycle from bars[from]: what v2 decides (brain != null), or
// with fixedGapAtr > 0 that gap always, on the v1 lot rule (base config).
[[nodiscard]] ReplayResult replayRulesV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int from,
                                         const BrainV2* brain, double fixedGapAtr, const BrainV2Config& config);

[[nodiscard]] BrainV2Training trainBrainV2(const std::vector<Bar>& bars, const std::vector<double>& spreads,
                                            const BrainV2& current, const BrainV2Config& config);

// Plain-text file format (no JSON dependency in core), one brain per file:
//   muuk-brain-v2 1
//   volLow <v> volHigh <v> trainedBars <n> trainedUntilMs <ms> forced <0|1>
//   gapAtrChoices <a> <b> ...
//   cells <81 lines: idx samples gapChoice lotMultiplier maxLevels exitFactor
//                    meanPnl sumPnl sumSquares locks lockRate meanLockLoss lockLossSum score>
[[nodiscard]] std::string serializeBrainV2(const BrainV2& brain);
[[nodiscard]] std::optional<BrainV2> parseBrainV2(const std::string& text);

// Online learning from one real closed basket. Updates only the cell that
// was traded, with a capped weight (one basket can move a cell's mean by
// at most ~1/(samples+1)), and can only veto: if the updated score is no
// longer positive, or the lock rate is over the limit, the cell becomes
// no-trade until a full re-train. It never changes a gap, lot or level.
void observeBasketOutcome(BrainV2& brain, const SituationV2& situation, const BrainV2Policy& used, double pnl, bool locked,
                          const BrainV2Config& config);

}  // namespace muuk::core
