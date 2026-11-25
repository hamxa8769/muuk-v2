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
#include <functional>
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

// Which stop a cycle opens with (Step 3, the brain's first order). The
// live engine arms exactly this: Both = a Buy Stop and a Sell Stop at
// once (the shipped behaviour); BuyFirst / SellFirst = one stop only,
// the other follows the fill; TrendFirst = the trend side first (EMA8 vs
// EMA21, the situation's trendDir): up -> Buy, down -> Sell, flat ->
// Both. Stored as a plain int in the cell and the file: -1 SellFirst,
// 0 Both, 1 BuyFirst, 2 TrendFirst. 0 is also the fallback for an
// undecided cell and for a brain file written before Step 3.
enum class BrainFirstSide : int { SellFirst = -1, Both = 0, BuyFirst = 1, TrendFirst = 2 };

// How the trainer picks the first side: Learn = one per cell, from that
// cell's own history (Step 3); the rest force one side everywhere, for
// the experiment's controls (Both reproduces the Step 2 numbers).
enum class BrainFirstSideMode : int { Learn = 0, Both = 1, BuyFirst = 2, SellFirst = 3, TrendFirst = 4 };

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
    // Kaufman Efficiency Ratio gate (the user's correction 2026-11-25: in a
    // clean trend the range strategy earns MORE, not less -- the market breaks
    // one side of the range and runs. The enemy is chop, not trend). ER is a
    // *gate feature* on the situation, never a new cell dimension (81 x 3 ER
    // buckets would starve the data). 0 = off: the shipped behaviour exactly.
    // 1 = block choppy cycles (ER < erChopBlock). 2 = 1 + a clean trend
    // (ER >= erTrendAllow) overrides the strong-trend block and the ATR
    // breaker -- trade trend-wise. 3 = 2 + the ATR percentile replaces the
    // fixed ATR breaker (block when atrPct > erAtrPctBlock, still overridable
    // by a clean trend). Thresholds are the research note's starting points
    // (ER > 0.5 trend, ER < 0.3 choppy); they are validated by the trainer,
    // never tuned into overfitting.
    int erMode = 0;
    double erChopBlock = 0.3;   // below this the market whipsaws: no new cycle (modes 1-3)
    double erTrendAllow = 0.5;  // at/above this the trend is clean: modes 2-3 trade trend-wise
    double erAtrPctBlock = 90.0;  // mode 3: block when the ATR percentile is above this
    int onlineMaxSamples = 500;        // online tally: older outcomes fade past this
    BrainFirstSideMode firstSideMode = BrainFirstSideMode::Learn;  // Step 3: Learn = one first side per cell, from
    // history (default); Both/BuyFirst/SellFirst/TrendFirst force one side everywhere (experiment controls)
    BrainRiskCaps caps;
    // Step 5: the live basket stop loss, in replay units (0 = off). When
    // > 0 the replay closes a basket the first bar its net reaches
    // -stopLoss, at the stop level, instead of running on to the lock --
    // so the trainer validates the brain under the same stop the live
    // basket closes at. 0 keeps every earlier experiment's numbers
    // exactly (the CLI default, and the shipped brain's training).
    double stopLoss = 0.0;
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
    int firstSide = 0;  // Step 3: the first order this cell learned (-1 Sell, 0 Both/undecided, 1 Buy, 2 trend);
    // the decision resolves 2 with the cycle's trendDir
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
    // The shift this brain was trained with (Step 2, decision D12): the
    // live runner uses these instead of the config's shift while a brain
    // with them loaded is driving the cycle. 0 = not recorded (a brain
    // file written before Step 2): the config shift stays in use.
    int shiftAfterFills = 0;  // fills per shift step, as trained
    double stepAtr = 0.0;     // each step widens the gap by this x the cycle-start ATR
    // Step 6: the gate mode and stop loss this brain trained under.
    // erMode -1 = not recorded (a brain file written before Step 6): the
    // live runner then falls back to the panel's erMode, as for the shift.
    // A recorded erMode wins live (like the trained shift): the brain was
    // validated under it. The stop loss is NOT overridden live -- it is a
    // risk control the panel governs; the recorded value is the audit
    // trail of what the brain trained under.
    int erMode = -1;       // Kaufman ER gate mode (0 off, 1 chop-block, 2 + clean-trend override, 3 + ATR percentile)
    double stopLoss = 0.0; // basket stop loss in replay units the brain trained under (0 = off / not recorded)
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
    double er = -1.0;       // Kaufman Efficiency Ratio on 5-min bars (n=20): ~1 clean trend, ~0 whipsaw, -1 unknown
    double atrPct = -1.0;   // current 1-min ATR ranked against its own values over the window (0-100), -1 unknown
    bool known = false;
};

// Why a cycle was refused, for the audit: the shadow replay counts what
// the strong-trend gate and the ATR breaker blocked, and whether those
// blocked cycles would have won. None = the brain said trade (or the caps
// let it through).
enum class BrainBlockReason { None, NotTrained, TooFewBars, AtrBreaker, CellNotTradable, StrongTrend, RecentLoss, VolumeSpike, Spread, ErChop, Caps };

struct BrainV2Decision {
    bool trade = false;
    BrainBlockReason blockedBy = BrainBlockReason::None;
    double gap = 0.0;
    int firstSide = 0;  // resolved first side for this cycle: -1 Sell Stop first, 0 both stops, +1 Buy Stop first
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
    bool ddGate = false;      // brain drawdown <= 50% of v1's (Step 5: reported per gate, so a failed run says why)
    bool lockGate = false;    // brain locks <= half of v1's
    std::string report;
};

// Kaufman Efficiency Ratio over the last n closes (oldest first):
// |close - close[n]| / sum(|change|). 1 = a straight move, 0 = pure whipsaw,
// -1 when there are too few bars. Computed on 5-min bars (see situationAtV2):
// 1-min ER is too noisy.
[[nodiscard]] double kaufmanEr(const std::deque<Bar>& bars, int n);

// The current Wilder ATR(period) ranked against its own values over the
// window (0-100). -1 when too few bars to rank.
[[nodiscard]] double atrPercentile(const std::deque<Bar>& bars, int period);

[[nodiscard]] SituationV2 situationAtV2(const std::deque<Bar>& oneMinuteBars, const BrainV2& brain, int recentOutcome = 0,
                                         double spread = 0.0, double medianSpread = 0.0);
[[nodiscard]] BrainV2Decision decideWithBrainV2(const BrainV2& brain, const SituationV2& situation, const BrainV2Config& config,
                                                 const BrainAccountState& account = {});

// Worst-case lock cost of a policy at this gap: alternating fills to the
// lock, both starting sides, the worse one. Per 0.01 lot start.
[[nodiscard]] double estimatedLockCost(double gap, const BrainV2Policy& policy);

// The lock cost the caps actually bound (Step 6): with a stop loss the
// basket closes at -stopLoss before it can lock, so the worst case is the
// stop, not the no-stop lock cost. Without a stop it is the estimate.
[[nodiscard]] double effectiveLockCost(double gap, const BrainV2Policy& policy, const BrainV2Config& config);

// A decision after the hard caps: downgraded (fewer levels / smaller lots)
// or refused. The brain's policy is never raised, only lowered.
[[nodiscard]] BrainV2Decision applyRiskCaps(BrainV2Decision decision, const BrainV2Config& config,
                                            const BrainAccountState& account);

// The first side a cell's stored value means for THIS cycle: TrendFirst
// (2) follows the cycle's trendDir (up -> +1, down -> -1, flat -> 0);
// BuyFirst (+1) / SellFirst (-1) / Both (0) pass through.
[[nodiscard]] int resolveFirstSide(int firstSide, int trendDir);

[[nodiscard]] ReplayCycleV2 replayCycleV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int start, double gap,
                                           double atr, const BrainV2Policy& policy, const BrainV2Config& config,
                                           int firstSide = 0);

[[nodiscard]] BrainV2 learnBrainV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int end,
                                    const BrainV2Config& config);

// Cycle after cycle from bars[from]: what v2 decides (brain != null), or
// with fixedGapAtr > 0 that gap always, on the v1 lot rule (base config).
[[nodiscard]] ReplayResult replayRulesV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int from,
                                         const BrainV2* brain, double fixedGapAtr, const BrainV2Config& config);

// Progress of a training run, for a UI to show while the background
// thread works: which stage ("walk-forward", "fixed", "v1", "held-out",
// "final") and how many of its periods are done. Null = no reporting.
using BrainTrainingProgress = std::function<void(const char* stage, int done, int total)>;

[[nodiscard]] BrainV2Training trainBrainV2(const std::vector<Bar>& bars, const std::vector<double>& spreads,
                                            const BrainV2& current, const BrainV2Config& config,
                                            BrainTrainingProgress progress = nullptr);

// One line for a failed run: which strict gate failed, with the numbers
// ("locks 14 > 10 (v1 20); held-out -12.34 <= 0"). "gates PASSED" when
// they passed. The gate definitions live in trainBrainV2; this only
// reports them.
[[nodiscard]] std::string gatesFailureSummary(const BrainV2Training& t);

// One shift combo tried by the trainer's shift search (Step 2): the walk-
// forward result for that (shiftAfterFills, stepAtr), and whether it passed
// the strict gates. The held-out is the crash proxy, so its locks outrank
// the total when ranking.
struct ShiftSearchCandidate {
    int shiftAfterFills = 0;
    double stepAtr = 0.0;
    bool gatesPassed = false;
    double totalPnl = 0.0;
    int locks = 0;
    double heldOutPnl = 0.0;
    int heldOutLocks = 0;
    int periodsUp = 0;
};

// Ranks one shift-search candidate ahead of another: gates-passing first,
// then fewest held-out locks, best held-out PnL, fewest walk-forward locks,
// best total.
[[nodiscard]] bool shiftComboBetter(const ShiftSearchCandidate& a, const ShiftSearchCandidate& b);

// Best gates-passing candidate, or nothing when no combo passed the gates
// (the shipped shift then stays -- the search never invents a pass).
[[nodiscard]] std::optional<ShiftSearchCandidate> pickBestShiftCombo(const std::vector<ShiftSearchCandidate>& candidates);

// Plain-text file format (no JSON dependency in core), one brain per file:
//   muuk-brain-v2 1
//   volLow <v> volHigh <v> trainedBars <n> trainedUntilMs <ms> forced <0|1> shiftAfterFills <n> stepAtr <x>
//   erMode <n> stopLoss <x>   (same header line, Step 6; an old file ends after stepAtr)
//   gapAtrChoices <a> <b> ...
//   cells <81 lines: idx samples gapChoice lotMultiplier maxLevels exitFactor
//                    meanPnl sumPnl sumSquares locks lockRate meanLockLoss lockLossSum score firstSide>
//   firstSide: -1 Sell first, 0 both stops (or undecided / an old file), 1 Buy first, 2 trend first
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
