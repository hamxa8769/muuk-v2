#include "muuk/core/market_brain_v2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

#include "muuk/core/regime_range.hpp"

namespace muuk::core {

namespace {

constexpr int kCells = 81;
constexpr int kWindowBars = 240;

int cellIndex(const Situation& s) { return s.volatility * 27 + s.trend * 9 + s.chop * 3 + s.session; }

std::deque<Bar> windowEndingAt(const std::vector<Bar>& bars, int last) {
    const int first = std::max(0, last - kWindowBars + 1);
    return std::deque<Bar>(bars.begin() + first, bars.begin() + last + 1);
}

double medianOf(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

struct Tally {
    int n = 0;
    double sum = 0.0;
    double squares = 0.0;
    int locks = 0;
    double lockLossSum = 0.0;
};

double scoreOf(const Tally& t) {
    if (t.n <= 0) return 0.0;
    const double mean = t.sum / t.n;
    const double variance = std::max(0.0, t.squares / t.n - mean * mean);
    return mean - std::sqrt(variance / t.n);
}

// The policies stage B chooses between, safest first: (mult, levels, exitFactor).
// Gap is chosen in stage A. 2.0/4 is the aggressive end -- the v1 lot rule with
// the levels capped, so the lock-cost caps can still hold it. A riskier policy
// is picked only if it scores clearly better (see the 0.05 margin in stage B).
// (A 6-level-only grid was tried 2026-11-24: the lock rate halved but the
// cycles doubled, so locks barely moved 48->44 and each lock cost ~2x more.)
const BrainV2Policy kCandidates[] = {
    {0, 1.0, 3, 1.0}, {0, 1.0, 4, 1.0}, {0, 1.25, 4, 0.5}, {0, 1.25, 4, 1.0},
    {0, 1.5, 6, 0.5}, {0, 1.0, 6, 1.0}, {0, 1.5, 3, 1.0}, {0, 2.0, 4, 1.0},
};

std::string lineOf(const char* name, const ReplayResult& r) {
    char buf[220];
    std::snprintf(buf, sizeof(buf), "%-14s cycles %5d  wins %5d  locks %4d  total %10.2f  worst cycle %9.2f  max drawdown %9.2f\n",
                  name, r.cycles, r.wins, r.locks, r.totalPnl, r.worstCycle, r.maxDrawdown);
    return buf;
}

}  // namespace

double estimatedLockCost(double gap, const BrainV2Policy& policy) {
    const double buyLine = gap / 2.0;
    const double sellLine = -gap / 2.0;
    double worst = 0.0;  // most negative net at the lock line
    for (const bool startBuy : {true, false}) {
        double net = 0.0;
        for (int i = 0; i < policy.maxLevels; ++i) {
            const bool buy = (i % 2 == 0) == startBuy;
            const double lot = std::pow(policy.lotMultiplier, static_cast<double>(i));
            const double price = buy ? buyLine : sellLine;
            (void)price;
            // Legs are held; the lock line is the next side's line.
            const bool lockBuy = (policy.maxLevels % 2 == 0) == startBuy;
            const double line = lockBuy ? buyLine : sellLine;
            net += (buy ? line - price : price - line) * lot;
        }
        worst = std::min(worst, net);
    }
    return worst < 0.0 ? -worst : 0.0;
}

// Step 6: the lock cost the caps bound. With a stop the worst case per cycle
// IS the stop, not the no-stop lock cost -- the stop is a hard, enforced,
// audited live cap (the engine closes the basket at the stop, and the
// audit force-closes if a target/stop was reached but the basket is still
// open), and the replay models it exactly (closes at -stopLoss, checked
// before the target). The $150 no-stop estimate is then not "conservative",
// it is a wrong bound: it downgrades policies the live basket would never
// lose $150 on, and leaves their edge (bigger targets at more levels, same
// -$50 worst case) on the table. Without a stop the estimate stands.
// (Step 6b: the stop defaults to OFF -- Step 6 proved it cuts the validated
// edge -- so the shipped brain trains with the no-stop estimate; the
// stop-aware bound still applies whenever a stop IS set.)
double effectiveLockCost(double gap, const BrainV2Policy& policy, const BrainV2Config& config) {
    const double worstCase = estimatedLockCost(gap, policy);
    return config.stopLoss > 0.0 ? std::min(worstCase, config.stopLoss) : worstCase;
}

// Kaufman Efficiency Ratio over the last n closes (oldest first):
// |close - close[n]| / sum(|change|). 1 = a straight move, 0 = pure whipsaw,
// -1 when there are too few bars.
double kaufmanEr(const std::deque<Bar>& bars, int n) {
    if (n <= 0 || static_cast<int>(bars.size()) < n + 1) return -1.0;
    const std::size_t last = bars.size() - 1;
    const std::size_t first = last - static_cast<std::size_t>(n);
    double path = 0.0;
    for (std::size_t i = first + 1; i <= last; ++i) path += std::fabs(bars[i].close - bars[i - 1].close);
    if (path <= 0.0) return -1.0;
    return std::fabs(bars[last].close - bars[first].close) / path;
}

// The current Wilder ATR(period) ranked against its own values over the
// window (0-100): the last value's share of ATRs at or below it. -1 when
// too few bars to rank. One pass: Wilder's smoothing is a running average.
double atrPercentile(const std::deque<Bar>& bars, int period) {
    if (period <= 0 || bars.size() < static_cast<std::size_t>(period) + 30) return -1.0;
    std::vector<double> atrs;
    atrs.reserve(bars.size());
    double sum = 0.0;
    double avg = 0.0;
    bool seeded = false;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const double tr = std::max({bars[i].high - bars[i].low, std::fabs(bars[i].high - bars[i - 1].close),
                                    std::fabs(bars[i].low - bars[i - 1].close)});
        if (!seeded) {
            sum += tr;
            if (i == static_cast<std::size_t>(period)) {
                avg = sum / period;
                atrs.push_back(avg);
                seeded = true;
            }
        } else {
            avg = (avg * (period - 1) + tr) / period;
            atrs.push_back(avg);
        }
    }
    if (atrs.size() < 30) return -1.0;
    const double last = atrs.back();
    std::size_t below = 0;
    for (const double v : atrs) {
        if (v <= last) ++below;
    }
    return 100.0 * static_cast<double>(below) / static_cast<double>(atrs.size());
}

SituationV2 situationAtV2(const std::deque<Bar>& oneMinuteBars, const BrainV2& brain, int recentOutcome, double spread,
                           double medianSpread) {
    SituationV2 out;
    out.recentOutcome = recentOutcome;
    out.spreadPct = medianSpread > 0.0 && spread > 0.0 ? spread / medianSpread : 0.0;
    MarketBrain tmp;
    tmp.trained = true;
    tmp.volLow = brain.volLow;
    tmp.volHigh = brain.volHigh;
    tmp.gapAtrChoices = brain.gapAtrChoices;
    out.base = situationAt(oneMinuteBars, tmp);
    if (!out.base.known) return out;
    const RegimeRead read = readRegime(oneMinuteBars, RegimeConfig{});
    if (read.regime == MarketRegime::Unknown) return out;
    out.known = true;
    // ATR trend: now vs the first three hours of the window.
    const std::size_t half = std::min(oneMinuteBars.size(), static_cast<std::size_t>(180));
    const std::deque<Bar> early(oneMinuteBars.begin(), oneMinuteBars.begin() + static_cast<std::ptrdiff_t>(half));
    const double atrEarlier = atrOf(early, 14);
    if (read.atrMinute > 0.0 && atrEarlier > 0.0) {
        out.atrTrend = read.atrMinute > atrEarlier * 1.15 ? 1 : (read.atrMinute < atrEarlier * 0.85 ? -1 : 0);
    }
    if (read.atrMinute > 0.0 && std::fabs(read.emaFast - read.emaSlow) >= 0.5 * read.atrMinute) {
        out.trendDir = read.emaFast > read.emaSlow ? 1 : -1;
    }
    out.rsiZone = read.rsi < 30.0 ? 0 : (read.rsi > 70.0 ? 2 : 1);
    const double close = oneMinuteBars.back().close;
    out.bollingerPos = close > read.bollingerUpper ? 2 : (close < read.bollingerLower ? 0 : 1);
    out.volume = read.volumeRatio < 0.7 ? 0 : (read.volumeRatio > 1.5 ? 2 : 1);
    // Kaufman ER on 5-minute bars: 1-min ER is too noisy, and ADX /
    // Choppiness already read 5-min (see the research doc). n=20 five-min
    // bars = 100 minutes. ATR percentile on the 1-minute window: the
    // current ATR ranked against its own last ~4 hours.
    out.er = kaufmanEr(aggregateBars(oneMinuteBars, 5), 20);
    out.atrPct = atrPercentile(oneMinuteBars, 14);
    return out;
}

BrainV2Decision applyRiskCaps(BrainV2Decision decision, const BrainV2Config& config, const BrainAccountState& account) {
    const BrainRiskCaps& caps = config.caps;
    if (!decision.trade) return decision;
    auto refuse = [&](const char* reason) {
        decision.trade = false;
        decision.blockedBy = BrainBlockReason::Caps;
        decision.why += std::string("; refused: ") + reason;
        return decision;
    };
    if (caps.maxDailyLoss > 0.0 && account.dailyPnl <= -caps.maxDailyLoss) return refuse("daily loss limit reached");
    if (caps.maxDrawdown > 0.0 && account.drawdown >= caps.maxDrawdown) return refuse("drawdown limit reached");
    if (caps.maxSpread > 0.0 && account.spread > caps.maxSpread) return refuse("spread above the cap");
    BrainV2Policy p = decision.policy;
    bool downgraded = false;
    while (p.maxLevels > caps.maxLevelsHard && p.maxLevels > 2) {
        --p.maxLevels;
        downgraded = true;
    }
    while (std::pow(p.lotMultiplier, static_cast<double>(std::max(0, p.maxLevels - 1))) > caps.maxSingleLot) {
        if (p.maxLevels > 2) {
            --p.maxLevels;
        } else if (p.lotMultiplier > 1.0) {
            p.lotMultiplier = 1.0;
        } else {
            break;
        }
        downgraded = true;
    }
    double lockCap = caps.maxLockCost;
    if (caps.maxLockCostPctOfBalance > 0.0 && account.balance > 0.0) {
        const double pctCap = account.balance * caps.maxLockCostPctOfBalance;
        lockCap = lockCap > 0.0 ? std::min(lockCap, pctCap) : pctCap;
    }
    if (lockCap > 0.0) {
        while (effectiveLockCost(decision.gap, p, config) > lockCap && p.maxLevels > 2) {
            --p.maxLevels;
            downgraded = true;
        }
        if (effectiveLockCost(decision.gap, p, config) > lockCap && p.lotMultiplier > 1.0) {
            p.lotMultiplier = 1.0;
            downgraded = true;
        }
        if (effectiveLockCost(decision.gap, p, config) > lockCap) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "lock cost %.2f over the cap %.2f even at 1.0x / 2 levels",
                          effectiveLockCost(decision.gap, p, config), lockCap);
            return refuse(buf);
        }
    }
    if (downgraded) {
        decision.policy = p;
        decision.estimatedLockCost = effectiveLockCost(decision.gap, p, config);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "; caps downgraded the policy to %.2fx / %d levels (lock cost %.2f)", p.lotMultiplier,
                      p.maxLevels, decision.estimatedLockCost);
        decision.why += buf;
    }
    return decision;
}

int resolveFirstSide(int firstSide, int trendDir) {
    if (firstSide == static_cast<int>(BrainFirstSide::TrendFirst)) {
        return trendDir > 0 ? 1 : (trendDir < 0 ? -1 : 0);
    }
    if (firstSide == static_cast<int>(BrainFirstSide::BuyFirst)) return 1;
    if (firstSide == static_cast<int>(BrainFirstSide::SellFirst)) return -1;
    return 0;  // Both, undecided, or an unknown value: arm both stops
}

BrainV2Decision decideWithBrainV2(const BrainV2& brain, const SituationV2& situation, const BrainV2Config& config,
                                   const BrainAccountState& account) {
    BrainV2Decision d;
    if (!brain.trained || brain.cells.size() != kCells) {
        d.why = "the v2 brain isn't trained";
        d.blockedBy = BrainBlockReason::NotTrained;
        return d;
    }
    if (!situation.known) {
        d.why = "too few bars to read this market";
        d.blockedBy = BrainBlockReason::TooFewBars;
        return d;
    }
    // A clean trend (high ER) overrides the volatility breaker: the range
    // strategy earns on a trend that breaks one side and runs. Only a choppy
    // extreme-volatility move is blocked. Modes 0-1 keep the shipped breaker.
    const bool cleanTrend = config.erMode >= 2 && situation.er >= config.erTrendAllow;
    bool overrodeBreaker = false;
    bool overrodeTrend = false;
    if (config.erMode != 3 && config.maxAtrVolMultiple > 0.0 && brain.volHigh > 0.0 &&
        situation.base.atr > config.maxAtrVolMultiple * brain.volHigh) {
        if (!cleanTrend) {
            char buf[300];
            std::snprintf(buf, sizeof(buf), "ATR %.2f is %.1fx the training high-vol threshold %.2f -- extreme volatility, no new cycle",
                          situation.base.atr, situation.base.atr / brain.volHigh, brain.volHigh);
            d.why = buf;
            d.blockedBy = BrainBlockReason::AtrBreaker;
            return d;
        }
        overrodeBreaker = true;
    }
    // Mode 3: the ATR percentile replaces the fixed breaker -- block only
    // when the ATR is in its own top decile over the window. A clean trend
    // still overrides it.
    if (config.erMode == 3 && situation.atrPct >= config.erAtrPctBlock && !cleanTrend) {
        char buf[300];
        std::snprintf(buf, sizeof(buf), "ATR percentile %.0f over %.0f -- extreme volatility for this window, no new cycle",
                      situation.atrPct, config.erAtrPctBlock);
        d.why = buf;
        d.blockedBy = BrainBlockReason::AtrBreaker;
        return d;
    }
    static constexpr const char* kLevels[] = {"low", "mid", "high"};
    static constexpr const char* kSessions[] = {"Asia", "London", "New York"};
    char where[200];
    std::snprintf(where, sizeof(where), "volatility %s, trend %s, chop %s, %s, ATR trend %+d, direction %+d",
                  kLevels[situation.base.volatility], kLevels[situation.base.trend], kLevels[situation.base.chop],
                  kSessions[situation.base.session], situation.atrTrend, situation.trendDir);
    const BrainV2Cell& cell = brain.cells[cellIndex(situation.base)];
    if (cell.best.gapChoice < 0 || cell.best.gapChoice >= static_cast<int>(brain.gapAtrChoices.size())) {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s: history shows no policy worth trading here (%d samples)", where, cell.samples);
        d.why = buf;
        d.blockedBy = BrainBlockReason::CellNotTradable;
        return d;
    }
    // Context gates: the extra features v2 sees.
    // ER chop-block (modes 1-3): a whipsaw market (low ER) fills both sides
    // and locks. It only fires when the cell would otherwise trade.
    if (config.erMode >= 1 && situation.er >= 0.0 && situation.er < config.erChopBlock) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: choppy market (Kaufman ER %.2f under %.2f), no new cycle", where, situation.er,
                      config.erChopBlock);
        d.why = buf;
        d.blockedBy = BrainBlockReason::ErChop;
        return d;
    }
    if (config.noTradeInStrongTrend && situation.base.trend == 2 && situation.trendDir != 0) {
        if (!cleanTrend) {
            d.why = std::string(where) + ": strong trend, no new range cycle";
            d.blockedBy = BrainBlockReason::StrongTrend;
            return d;
        }
        overrodeTrend = true;
    }
    if (situation.recentOutcome < 0 && cell.score < config.recentLossScore) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: last basket lost or locked, and this state's score is only %.2f", where, cell.score);
        d.why = buf;
        d.blockedBy = BrainBlockReason::RecentLoss;
        return d;
    }
    if (situation.volume == 2 && cell.score < config.highVolumeScore) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: tick-volume spike, and this state's score is only %.2f", where, cell.score);
        d.why = buf;
        d.blockedBy = BrainBlockReason::VolumeSpike;
        return d;
    }
    if (config.maxSpreadMultiple > 0.0 && situation.spreadPct > config.maxSpreadMultiple) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: spread %.1fx its median, too wide to trade", where, situation.spreadPct);
        d.why = buf;
        d.blockedBy = BrainBlockReason::Spread;
        return d;
    }
    d.trade = true;
    d.policy = cell.best;
    d.gap = clampedGap(brain.gapAtrChoices[cell.best.gapChoice], situation.base.atr, config.base);
    d.firstSide = resolveFirstSide(cell.firstSide, situation.trendDir);
    d.lockRisk = cell.lockRate;
    d.estimatedLockCost = effectiveLockCost(d.gap, d.policy, config);
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s: gap %.2f (%.1f x ATR %.2f), %.2fx / %d levels, exit x%.2f, %d samples, mean %+.2f, lock rate %.1f%%, "
                  "lock cost %.2f, first %s",
                  where, d.gap, brain.gapAtrChoices[cell.best.gapChoice], situation.base.atr, d.policy.lotMultiplier,
                  d.policy.maxLevels, d.policy.exitFactor, cell.samples, cell.meanPnl, 100.0 * cell.lockRate,
                  d.estimatedLockCost, d.firstSide > 0 ? "Buy" : (d.firstSide < 0 ? "Sell" : "both"));
    d.why = buf;
    if (overrodeBreaker || overrodeTrend) {
        const char* what = overrodeBreaker && overrodeTrend ? "ATR breaker + strong-trend gate"
                           : overrodeBreaker                 ? "ATR breaker"
                                                             : "strong-trend gate";
        char note[160];
        std::snprintf(note, sizeof(note), "; clean trend (ER %.2f) overrode the %s", situation.er, what);
        d.why += note;
    }
    return applyRiskCaps(d, config, account);
}

ReplayCycleV2 replayCycleV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int start, double gap, double atr,
                             const BrainV2Policy& policy, const BrainV2Config& config, int firstSide) {
    struct Leg {
        bool buy;
        double price;
        double lot;
    };
    ReplayCycleV2 out;
    const double mid = bars[start].close;
    double buyLine = mid + gap / 2.0;
    double sellLine = mid - gap / 2.0;
    // Centered on the tick that starts the cycle. Both chart-based
    // placements lost in backtest (center - $894.98 / 48 locks; edges
    // - $525.11 / 55 locks; centered + $1,230.65 / 48), so the replay
    // stays centered. The live toggle defaults off (see regimeRanges).
    std::vector<Leg> legs;
    double costs = 0.0;
    bool nextBuy = true;

    auto netAt = [&](double price) {
        double net = -costs;
        for (const Leg& l : legs) net += (l.buy ? price - l.price : l.price - price) * l.lot;
        return net;
    };
    const int end = std::min(static_cast<int>(bars.size()) - 1, start + config.base.maxCycleBars);
    for (int j = start + 1; j <= end; ++j) {
        const Bar& bar = bars[j];
        const double spread = static_cast<std::size_t>(j) < spreads.size() ? spreads[j] : 0.0;
        int fillsThisBar = 0;
        while (fillsThisBar < 2) {
            // Step 3: the first order. Both (0) arms both stops, as
            // before; Buy (+1) arms only the Buy Stop and Sell (-1) only
            // the Sell Stop until the first fill, when the opposite side
            // arms (the engine's StartSide rule). A Trend value (2) must
            // be resolved by the caller (see resolveFirstSide); here any
            // positive value reads as Buy-first, any negative as Sell-first.
            const bool buyHit = (legs.empty() ? firstSide >= 0 : nextBuy) && bar.high >= buyLine;
            const bool sellHit = (legs.empty() ? firstSide <= 0 : !nextBuy) && bar.low <= sellLine;
            if (!buyHit && !sellHit) break;
            const bool buy = buyHit && (!sellHit || bar.close >= mid);
            if (static_cast<int>(legs.size()) >= policy.maxLevels) {
                const double line = buy ? buyLine : sellLine;
                out.pnl = netAt(line) - spread * std::fabs(netAt(1.0) - netAt(0.0));
                out.endBar = j;
                out.fills = static_cast<int>(legs.size());
                out.locked = true;
                return out;
            }
            const double lot = std::pow(policy.lotMultiplier, static_cast<double>(legs.size()));
            legs.push_back(Leg{buy, buy ? buyLine : sellLine, lot});
            costs += (spread + config.slippage + config.commission) * lot;
            nextBuy = !buy;
            ++fillsThisBar;
            if (config.base.shiftAfterFills > 0 && static_cast<int>(legs.size()) % config.base.shiftAfterFills == 0) {
                const double gapNow = buyLine - sellLine;
                const double wider = config.base.stepAtr > 0.0
                                         ? std::min(config.base.maxGap, std::max(gapNow + config.base.stepAtr * atr, gapNow + 1.0))
                                         : gapNow;
                const double center = (buyLine + sellLine) / 2.0;
                buyLine = center + wider / 2.0;
                sellLine = center - wider / 2.0;
            }
        }
        if (legs.empty()) continue;
        // Step 5: the live basket stop loss. The first bar the basket's
        // worst net reaches -stopLoss, the basket stops out at the stop
        // level instead of running on to the lock (a stop close, not a
        // lock). Checked before the target: on a bar that touches both,
        // the loss is taken -- the conservative reading, since a 1-minute
        // bar does not say which level was touched first. 0 = off.
        if (config.stopLoss > 0.0) {
            const double worst = std::min(netAt(bar.high), netAt(bar.low));
            if (worst <= -config.stopLoss) {
                out.pnl = -config.stopLoss;
                out.endBar = j;
                out.fills = static_cast<int>(legs.size());
                return out;
            }
        }
        const double target = legs.back().lot * gap / 2.0 * policy.exitFactor;
        const double best = std::max(netAt(bar.high), netAt(bar.low));
        if (fillsThisBar == 0 && best >= target) {
            out.pnl = target;
            out.endBar = j;
            out.fills = static_cast<int>(legs.size());
            return out;
        }
    }
    out.pnl = legs.empty() ? 0.0 : netAt(bars[end].close);
    out.endBar = end;
    out.fills = static_cast<int>(legs.size());
    return out;
}

BrainV2 learnBrainV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int end, const BrainV2Config& config) {
    BrainV2 brain;
    brain.gapAtrChoices = config.base.gapAtrChoices;
    std::vector<double> atrs;
    for (int i = kWindowBars; i < end; i += config.base.stepBars) atrs.push_back(atrOf(windowEndingAt(bars, i), 14));
    if (atrs.size() < 100) return brain;
    std::sort(atrs.begin(), atrs.end());
    brain.volLow = atrs[atrs.size() / 3];
    brain.volHigh = atrs[atrs.size() * 2 / 3];

    MarketBrain tmp;
    tmp.trained = true;
    tmp.volLow = brain.volLow;
    tmp.volHigh = brain.volHigh;
    tmp.gapAtrChoices = brain.gapAtrChoices;

    const std::size_t choices = config.base.gapAtrChoices.size();
    // Stage A: the gap, on the v1 lot rule (base lotMultiplier / maxPositions).
    // A flat policy hides the edge this strategy actually trades on — the
    // lot growth on winning cycles — so the gap is learned the way v1
    // learned it, and stage B then picks the safest policy at that gap.
    std::vector<Tally> tallyA(kCells * choices);
    for (int i = kWindowBars; i + 1 < end; i += config.base.stepBars) {
        const Situation s = situationAt(windowEndingAt(bars, i), tmp);
        if (!s.known) continue;
        BrainV2Config bounded = config;
        bounded.base.maxCycleBars = std::min(config.base.maxCycleBars, end - 1 - i);
        for (std::size_t c = 0; c < choices; ++c) {
            const double gap = clampedGap(config.base.gapAtrChoices[c], s.atr, config.base);
            const BrainV2Policy v1rule{static_cast<int>(c), config.base.lotMultiplier, config.base.maxPositions, 1.0};
            const ReplayCycleV2 r = replayCycleV2(bars, spreads, i, gap, s.atr, v1rule, bounded);
            Tally& t = tallyA[cellIndex(s) * choices + c];
            ++t.n;
            t.sum += r.pnl;
            t.squares += r.pnl * r.pnl;
            if (r.locked) {
                ++t.locks;
                t.lockLossSum += std::fabs(r.pnl);
            }
        }
    }
    std::vector<int> bestGap(kCells, -1);
    for (int cell = 0; cell < kCells; ++cell) {
        double bestScore = 0.0;
        for (std::size_t c = 0; c < choices; ++c) {
            const Tally& t = tallyA[cell * choices + c];
            if (t.n < config.base.minSamples) continue;
            const double score = scoreOf(t);
            if (score > bestScore) {
                bestScore = score;
                bestGap[cell] = static_cast<int>(c);
            }
        }
    }
    // Stage B: the policy, for the gap stage A picked, with the lock-risk check.
    constexpr std::size_t kPolicies = sizeof(kCandidates) / sizeof(kCandidates[0]);
    std::vector<Tally> tallyB(kCells * kPolicies);
    for (int i = kWindowBars; i + 1 < end; i += config.base.stepBars) {
        const Situation s = situationAt(windowEndingAt(bars, i), tmp);
        if (!s.known) continue;
        const int cell = cellIndex(s);
        if (bestGap[cell] < 0) continue;
        BrainV2Config bounded = config;
        bounded.base.maxCycleBars = std::min(config.base.maxCycleBars, end - 1 - i);
        const double gap = clampedGap(config.base.gapAtrChoices[bestGap[cell]], s.atr, config.base);
        for (std::size_t k = 0; k < kPolicies; ++k) {
            BrainV2Policy p = kCandidates[k];
            p.gapChoice = bestGap[cell];
            const ReplayCycleV2 r = replayCycleV2(bars, spreads, i, gap, s.atr, p, bounded);
            Tally& t = tallyB[cell * kPolicies + k];
            ++t.n;
            t.sum += r.pnl;
            t.squares += r.pnl * r.pnl;
            if (r.locked) {
                ++t.locks;
                t.lockLossSum += std::fabs(r.pnl);
            }
        }
    }
    brain.cells.assign(kCells, BrainV2Cell{});
    for (int cell = 0; cell < kCells; ++cell) {
        BrainV2Cell& out = brain.cells[cell];
        int samples = 0;
        for (std::size_t c = 0; c < choices; ++c) samples = std::max(samples, tallyA[cell * choices + c].n);
        out.samples = samples;
        if (bestGap[cell] < 0) continue;
        double bestScore = 0.0;
        for (std::size_t k = 0; k < kPolicies; ++k) {
            const Tally& t = tallyB[cell * kPolicies + k];
            if (t.n < config.base.minSamples) continue;
            const double shrunkLockRate = (t.locks + 1.0) / (t.n + 10.0);
            if (shrunkLockRate > config.maxLockRate) continue;
            const double score = scoreOf(t);
            // A riskier policy must beat the safer one already picked by
            // more than 0.05, or the safer one stays.
            if (score > bestScore + 0.05) {
                bestScore = score;
                out.best = kCandidates[k];
                out.best.gapChoice = bestGap[cell];
                out.samples = t.n;
                out.meanPnl = t.sum / t.n;
                out.sumPnl = t.sum;
                out.sumSquares = t.squares;
                out.locks = t.locks;
                out.lockRate = t.n > 0 ? static_cast<double>(t.locks) / t.n : 0.0;
                out.meanLockLoss = t.locks > 0 ? t.lockLossSum / t.locks : 0.0;
                out.lockLossSum = t.lockLossSum;
                out.score = score;
            }
        }
    }
    // Stage C (Step 3): the first order, per cell. For every tradable
    // cell, replay its cycles four ways -- both stops armed (Both), only
    // the Buy Stop first, only the Sell Stop first, and the trend side
    // first (the direction the cycle's own EMA8/EMA21 points, so the
    // first side is trend-wise, not a fixed Buy/Sell the bull market
    // would have taught it). A single side must beat Both by more than
    // 0.05 -- the same margin stage B demands of a riskier policy -- or
    // Both stays: in a range the first fill is whichever line the price
    // touches first, and waiting for one side is only worth it when the
    // history says so. Forced modes (the experiment's controls) skip the
    // tallies and stamp the forced side on every tradable cell, so the
    // walk-forward and the saved brain trade exactly what was validated.
    if (config.firstSideMode == BrainFirstSideMode::Learn) {
        constexpr std::size_t kFirstSides = 4;  // 0 Both, 1 Buy, 2 Sell, 3 Trend
        std::vector<Tally> tallyC(kCells * kFirstSides);
        for (int i = kWindowBars; i + 1 < end; i += config.base.stepBars) {
            const Situation s = situationAt(windowEndingAt(bars, i), tmp);
            if (!s.known) continue;
            const int cell = cellIndex(s);
            const BrainV2Cell& chosen = brain.cells[cell];
            if (chosen.best.gapChoice < 0) continue;
            BrainV2Config bounded = config;
            bounded.base.maxCycleBars = std::min(config.base.maxCycleBars, end - 1 - i);
            const double gap = clampedGap(config.base.gapAtrChoices[chosen.best.gapChoice], s.atr, config.base);
            const SituationV2 s2 = situationAtV2(windowEndingAt(bars, i), brain, 0, 0.0, 0.0);
            const int trendSide = s2.trendDir > 0 ? 1 : (s2.trendDir < 0 ? -1 : 0);
            // Three replays; Trend reuses one of them (it IS that side
            // when the trend is clear, and Both when it is flat).
            const ReplayCycleV2 rBoth = replayCycleV2(bars, spreads, i, gap, s.atr, chosen.best, bounded, 0);
            const ReplayCycleV2 rBuy = replayCycleV2(bars, spreads, i, gap, s.atr, chosen.best, bounded, 1);
            const ReplayCycleV2 rSell = replayCycleV2(bars, spreads, i, gap, s.atr, chosen.best, bounded, -1);
            const ReplayCycleV2* results[kFirstSides] = {&rBoth, &rBuy, &rSell,
                                                          trendSide > 0 ? &rBuy : (trendSide < 0 ? &rSell : &rBoth)};
            for (std::size_t v = 0; v < kFirstSides; ++v) {
                Tally& t = tallyC[cell * kFirstSides + v];
                ++t.n;
                t.sum += results[v]->pnl;
                t.squares += results[v]->pnl * results[v]->pnl;
                if (results[v]->locked) {
                    ++t.locks;
                    t.lockLossSum += std::fabs(results[v]->pnl);
                }
            }
        }
        for (int cell = 0; cell < kCells; ++cell) {
            BrainV2Cell& out = brain.cells[cell];
            if (out.best.gapChoice < 0) continue;
            const Tally& both = tallyC[cell * kFirstSides + 0];
            const double bothScore = scoreOf(both);
            int bestVariant = 0;
            double bestScore = bothScore;
            for (std::size_t v = 1; v < kFirstSides; ++v) {
                const Tally& t = tallyC[cell * kFirstSides + v];
                if (t.n < config.base.minSamples) continue;
                const double shrunkLockRate = (t.locks + 1.0) / (t.n + 10.0);
                if (shrunkLockRate > config.maxLockRate) continue;
                const double score = scoreOf(t);
                if (score > bestScore) {
                    bestScore = score;
                    bestVariant = static_cast<int>(v);
                }
            }
            if (bestVariant != 0 && bestScore > bothScore + 0.05) {
                out.firstSide = bestVariant == 1 ? static_cast<int>(BrainFirstSide::BuyFirst)
                                : bestVariant == 2 ? static_cast<int>(BrainFirstSide::SellFirst)
                                                   : static_cast<int>(BrainFirstSide::TrendFirst);
            }
        }
    } else {
        const int forced = config.firstSideMode == BrainFirstSideMode::BuyFirst
                               ? static_cast<int>(BrainFirstSide::BuyFirst)
                           : config.firstSideMode == BrainFirstSideMode::SellFirst
                               ? static_cast<int>(BrainFirstSide::SellFirst)
                           : config.firstSideMode == BrainFirstSideMode::TrendFirst
                               ? static_cast<int>(BrainFirstSide::TrendFirst)
                               : static_cast<int>(BrainFirstSide::Both);
        for (BrainV2Cell& out : brain.cells) {
            if (out.best.gapChoice >= 0) out.firstSide = forced;
        }
    }
    brain.trained = true;
    brain.trainedBars = end;
    brain.trainedUntilMs = bars[end - 1].startMs;
    // Step 2 (decision D12): the shift this brain was trained with goes
    // into the file, so the live runner trades the validated shift, not
    // whatever the operator's config happens to say.
    brain.shiftAfterFills = config.base.shiftAfterFills;
    brain.stepAtr = config.base.stepAtr;
    // Step 6: the same for the ER gate mode and the stop loss -- the live
    // runner uses the brain's recorded erMode (see BrainKeeperV2), and the
    // recorded stop loss is the audit trail of what it trained under.
    brain.erMode = config.erMode;
    brain.stopLoss = config.stopLoss;
    return brain;
}

ReplayResult replayRulesV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int from, const BrainV2* brain,
                            double fixedGapAtr, const BrainV2Config& config) {
    ReplayResult out;
    double equity = 0.0;
    double peak = 0.0;
    double dailyPnl = 0.0;
    int64_t day = -1;
    int recentOutcome = 0;
    const int last = static_cast<int>(bars.size()) - 1;
    std::vector<double> spreadWindow;
    for (int i = from; i <= last; ++i) {
        if (static_cast<std::size_t>(i) < spreads.size()) spreadWindow.push_back(spreads[i]);
    }
    const double medianSpread = medianOf(spreadWindow);
    for (int i = from; i < last;) {
        const std::deque<Bar> window = windowEndingAt(bars, i);
        double gap = 0.0;
        double atr = 0.0;
        BrainV2Policy policy;
        int firstSide = 0;  // Step 3: the brain's first order for this cycle; the fixed path stays Both
        if (fixedGapAtr > 0.0) {
            atr = atrOf(window, 14);
            gap = atr > 0.0 ? clampedGap(fixedGapAtr, atr, config.base) : 0.0;
            policy = BrainV2Policy{-1, config.base.lotMultiplier, config.base.maxPositions, 1.0};
        } else if (brain) {
            const int64_t thisDay = bars[i].startMs / 86'400'000LL;
            if (day < 0) day = thisDay;
            if (thisDay != day) {
                day = thisDay;
                dailyPnl = 0.0;
            }
            const double spread = static_cast<std::size_t>(i) < spreads.size() ? spreads[i] : 0.0;
            const SituationV2 s = situationAtV2(window, *brain, recentOutcome, spread, medianSpread);
            const BrainAccountState account{dailyPnl, peak - equity, spread, 0.0};
            const BrainV2Decision d = decideWithBrainV2(*brain, s, config, account);
            if (d.trade) {
                gap = d.gap;
                atr = s.base.atr;
                policy = d.policy;
                firstSide = d.firstSide;
            }
            if (!d.trade &&
                (d.blockedBy == BrainBlockReason::StrongTrend || d.blockedBy == BrainBlockReason::AtrBreaker)) {
                // Shadow audit: what the cell would have traded, replayed
                // for the record only. Never counted in the totals, never
                // moves the loop: the real walk steps on by stepBars below.
                const BrainV2Cell& cell = brain->cells[cellIndex(s.base)];
                if (cell.best.gapChoice >= 0 && cell.best.gapChoice < static_cast<int>(brain->gapAtrChoices.size())) {
                    BrainV2Decision shadow;
                    shadow.trade = true;
                    shadow.gap = clampedGap(brain->gapAtrChoices[cell.best.gapChoice], s.base.atr, config.base);
                    shadow.policy = cell.best;
                    shadow = applyRiskCaps(shadow, config, account);
                    if (shadow.trade) {
                        BrainV2Config bounded = config;
                        bounded.base.maxCycleBars = std::min(config.base.maxCycleBars, last - i);
                        const ReplayCycleV2 r = replayCycleV2(bars, spreads, i, shadow.gap, s.base.atr, shadow.policy,
                                                              bounded, resolveFirstSide(cell.firstSide, s.trendDir));
                        if (d.blockedBy == BrainBlockReason::StrongTrend) {
                            ++out.shadowTrendBlocked;
                            out.shadowTrendPnl += r.pnl;
                            if (r.pnl > 0.0) ++out.shadowTrendWins;
                            if (r.locked) ++out.shadowTrendLocks;
                        } else {
                            ++out.shadowBreakerBlocked;
                            out.shadowBreakerPnl += r.pnl;
                            if (r.pnl > 0.0) ++out.shadowBreakerWins;
                            if (r.locked) ++out.shadowBreakerLocks;
                        }
                    }
                }
            }
        }
        if (gap <= 0.0) {
            i += config.base.stepBars;
            recentOutcome = 0;  // the cooldown is one decision, not the rest of the period
            continue;
        }
        BrainV2Config bounded = config;
        bounded.base.maxCycleBars = std::min(config.base.maxCycleBars, last - i);
        const ReplayCycleV2 r = replayCycleV2(bars, spreads, i, gap, atr, policy, bounded, firstSide);
        ++out.cycles;
        if (r.pnl > 0.0) ++out.wins;
        if (r.locked) ++out.locks;
        out.totalPnl += r.pnl;
        out.worstCycle = std::min(out.worstCycle, r.pnl);
        equity += r.pnl;
        dailyPnl += r.pnl;
        peak = std::max(peak, equity);
        out.maxDrawdown = std::max(out.maxDrawdown, peak - equity);
        recentOutcome = r.locked ? -1 : (r.pnl > 0.0 ? 1 : -1);
        i = std::max(r.endBar, i + 1);
    }
    return out;
}

namespace {

WalkForwardResult walkForwardV2Impl(const std::vector<Bar>& bars, const std::vector<double>& spreads, const BrainV2* brain,
                                    bool learnEachPeriod, const BrainV2Config& config, const char* stage = "walk-forward",
                                    const BrainTrainingProgress& progress = nullptr) {
    WalkForwardResult out;
    const int total = static_cast<int>(bars.size());
    const int first = std::max(kWindowBars * 2, static_cast<int>(total * config.base.firstLearnShare));
    const int length = std::max(kWindowBars * 2, (total - first) / std::max(1, config.base.periods));
    double equity = 0.0;
    double peak = 0.0;
    for (int from = first; from + kWindowBars < total; from += length) {
        const int to = std::min(total, from + length);
        const std::vector<Bar> upTo(bars.begin(), bars.begin() + to);
        const std::vector<double> spreadsUpTo(spreads.begin(), spreads.begin() + std::min<std::size_t>(spreads.size(), to));
        ReplayResult r;
        if (learnEachPeriod) {
            const BrainV2 learned = learnBrainV2(bars, spreads, from, config);
            if (!learned.trained) continue;
            r = replayRulesV2(upTo, spreadsUpTo, from, &learned, 0.0, config);
        } else {
            r = replayRulesV2(upTo, spreadsUpTo, from, brain, brain ? 0.0 : 3.0, config);
        }
        ++out.periods;
        if (progress) progress(stage, out.periods, config.base.periods);
        out.periodsUp += r.totalPnl > 0.0 ? 1 : 0;
        out.total.cycles += r.cycles;
        out.total.wins += r.wins;
        out.total.locks += r.locks;
        out.total.shadowTrendBlocked += r.shadowTrendBlocked;
        out.total.shadowTrendPnl += r.shadowTrendPnl;
        out.total.shadowTrendWins += r.shadowTrendWins;
        out.total.shadowTrendLocks += r.shadowTrendLocks;
        out.total.shadowBreakerBlocked += r.shadowBreakerBlocked;
        out.total.shadowBreakerPnl += r.shadowBreakerPnl;
        out.total.shadowBreakerWins += r.shadowBreakerWins;
        out.total.shadowBreakerLocks += r.shadowBreakerLocks;
        out.total.worstCycle = std::min(out.total.worstCycle, r.worstCycle);
        out.total.maxDrawdown = std::max(out.total.maxDrawdown, peak - equity + r.maxDrawdown);
        equity += r.totalPnl;
        peak = std::max(peak, equity);
    }
    out.total.totalPnl = equity;
    return out;
}

}  // namespace

BrainV2Training trainBrainV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, const BrainV2& current,
                              const BrainV2Config& config, BrainTrainingProgress progress) {
    BrainV2Training t;
    const int total = static_cast<int>(bars.size());
    const int first = static_cast<int>(total * config.base.firstLearnShare);
    if (first < kWindowBars * 2 || total - first < kWindowBars * 2) {
        t.report = "too few bars to learn from: " + std::to_string(total) + "\n";
        return t;
    }
    t.tested = learnBrainV2(bars, spreads, first, config);
    if (!t.tested.trained) {
        t.report = "too few bars to learn from: " + std::to_string(total) + "\n";
        return t;
    }
    const WalkForwardResult v2 = walkForwardV2Impl(bars, spreads, nullptr, true, config, "walk-forward", progress);
    const WalkForwardResult fixed = walkForwardV2Impl(bars, spreads, nullptr, false, config, "fixed", progress);
    const WalkForwardResult v1 = walkForward(bars, spreads, nullptr, true, config.base);
    t.brainTest = v2.total;
    t.fixedTest = fixed.total;
    t.v1Test = v1.total;
    t.periods = v2.periods;
    t.brainPeriodsUp = v2.periodsUp;

    // Held out: the last 15% is never trained on.
    const int heldFrom = total * 85 / 100;
    const BrainV2 heldBrain = learnBrainV2(bars, spreads, heldFrom, config);
    if (heldBrain.trained) {
        t.heldOutV2 = replayRulesV2(bars, spreads, heldFrom, &heldBrain, 0.0, config);
        t.heldOutFixed = replayRulesV2(bars, spreads, heldFrom, nullptr, 3.0, config);
        const MarketBrain heldV1 = learnBrain(bars, spreads, heldFrom, config.base);
        if (heldV1.trained) t.heldOutV1 = replayRules(bars, spreads, heldFrom, &heldV1, 0.0, config.base);
    }
    if (progress) progress("held-out", 1, 1);

    t.better = t.brainTest.cycles > 0 && t.brainTest.totalPnl > 0.0 && t.brainTest.totalPnl > t.fixedTest.totalPnl &&
               t.brainTest.totalPnl > t.v1Test.totalPnl && t.brainTest.maxDrawdown <= t.fixedTest.maxDrawdown;
    t.ddGate = t.v1Test.maxDrawdown <= 0.0 ? t.brainTest.maxDrawdown <= 0.0
                                           : t.brainTest.maxDrawdown <= 0.5 * t.v1Test.maxDrawdown;
    t.lockGate = t.v1Test.locks <= 0 ? t.brainTest.locks <= 0 : t.brainTest.locks <= t.v1Test.locks / 2;
    t.gatesPassed = t.better && t.ddGate && t.lockGate && t.brainPeriodsUp >= 5 && t.heldOutV2.totalPnl > 0.0;
    if (t.better) {
        if (progress) progress("final", 0, 1);
        t.brain = learnBrainV2(bars, spreads, total, config);
        if (progress) progress("final", 1, 1);
    }

    char head[240];
    std::snprintf(head, sizeof(head),
                  "bars %d, walking forward over %d periods after the first %d (per 0.01 lot start); "
                  "v2 up in %d; v1 numbers are without slippage/commission, so v2-vs-v1 is conservative for v2\n",
                  total, v2.periods, first, v2.periodsUp);
    t.report = head + lineOf("v2 brain", t.brainTest) + lineOf("fixed 3 x ATR", t.fixedTest) + lineOf("v1 brain", t.v1Test) +
               lineOf("held-out v2", t.heldOutV2) + lineOf("held-out fixed", t.heldOutFixed) + lineOf("held-out v1", t.heldOutV1);
    {
        char shadow[800];
        std::snprintf(shadow, sizeof(shadow),
                      "shadow audit (blocked cycles replayed anyway, NOT counted above):\n"
                      "  strong-trend gate blocked %d cycles: shadow total %+10.2f, %d wins, %d locks\n"
                      "  ATR breaker blocked %d cycles: shadow total %+10.2f, %d wins, %d locks\n"
                      "  held-out: trend gate %d (shadow %+10.2f, %d wins, %d locks), breaker %d (shadow %+10.2f, %d wins, %d locks)\n",
                      t.brainTest.shadowTrendBlocked, t.brainTest.shadowTrendPnl, t.brainTest.shadowTrendWins,
                      t.brainTest.shadowTrendLocks, t.brainTest.shadowBreakerBlocked, t.brainTest.shadowBreakerPnl,
                      t.brainTest.shadowBreakerWins, t.brainTest.shadowBreakerLocks, t.heldOutV2.shadowTrendBlocked,
                      t.heldOutV2.shadowTrendPnl, t.heldOutV2.shadowTrendWins, t.heldOutV2.shadowTrendLocks,
                      t.heldOutV2.shadowBreakerBlocked, t.heldOutV2.shadowBreakerPnl, t.heldOutV2.shadowBreakerWins,
                      t.heldOutV2.shadowBreakerLocks);
        t.report += shadow;
    }
    t.report += t.better ? "result: better walking forward -- adopted\n" : "result: not better -- not adopted\n";
    t.report += t.gatesPassed ? "gates: PASSED (drawdown <= 50% of v1, locks <= 50% of v1, >=5 periods up, held-out > 0)\n"
                              : "gates: NOT PASSED\n";
    (void)current;
    return t;
}

std::string gatesFailureSummary(const BrainV2Training& t) {
    if (t.gatesPassed) return "gates PASSED";
    std::vector<std::string> failed;
    if (!t.better) {
        char buf[220];
        std::snprintf(buf, sizeof(buf), "walk-forward not better (v2 %+.2f / %d cycles vs fixed %+.2f, v1 %+.2f)",
                      t.brainTest.totalPnl, t.brainTest.cycles, t.fixedTest.totalPnl, t.v1Test.totalPnl);
        failed.push_back(buf);
    }
    if (!t.ddGate) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "drawdown %.2f over 50%% of v1's %.2f", t.brainTest.maxDrawdown,
                      t.v1Test.maxDrawdown);
        failed.push_back(buf);
    }
    if (!t.lockGate) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "locks %d over half of v1's %d", t.brainTest.locks, t.v1Test.locks);
        failed.push_back(buf);
    }
    if (t.brainPeriodsUp < 5) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "up in %d of %d periods (< 5)", t.brainPeriodsUp, t.periods);
        failed.push_back(buf);
    }
    if (t.heldOutV2.totalPnl <= 0.0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "held-out %+.2f / %d locks (must be > 0)", t.heldOutV2.totalPnl,
                      t.heldOutV2.locks);
        failed.push_back(buf);
    }
    std::string out = "gates NOT PASSED: ";
    for (std::size_t i = 0; i < failed.size(); ++i) {
        if (i > 0) out += "; ";
        out += failed[i];
    }
    return out;
}

bool shiftComboBetter(const ShiftSearchCandidate& a, const ShiftSearchCandidate& b) {
    if (a.gatesPassed != b.gatesPassed) return a.gatesPassed;
    if (a.heldOutLocks != b.heldOutLocks) return a.heldOutLocks < b.heldOutLocks;
    if (a.heldOutPnl != b.heldOutPnl) return a.heldOutPnl > b.heldOutPnl;
    if (a.locks != b.locks) return a.locks < b.locks;
    return a.totalPnl > b.totalPnl;
}

std::optional<ShiftSearchCandidate> pickBestShiftCombo(const std::vector<ShiftSearchCandidate>& candidates) {
    std::optional<ShiftSearchCandidate> best;
    for (const ShiftSearchCandidate& c : candidates) {
        if (!c.gatesPassed) continue;
        if (!best.has_value() || shiftComboBetter(c, *best)) best = c;
    }
    return best;
}

std::string serializeBrainV2(const BrainV2& brain) {
    std::ostringstream out;
    out << "muuk-brain-v2 1\n";
    out << "volLow " << brain.volLow << " volHigh " << brain.volHigh << " trainedBars " << brain.trainedBars << " trainedUntilMs "
        << brain.trainedUntilMs << " forced " << (brain.forced ? 1 : 0) << " shiftAfterFills " << brain.shiftAfterFills
        << " stepAtr " << brain.stepAtr << " erMode " << brain.erMode << " stopLoss " << brain.stopLoss << "\n";
    out << "gapAtrChoices";
    for (const double g : brain.gapAtrChoices) out << ' ' << g;
    out << "\n";
    for (std::size_t i = 0; i < brain.cells.size(); ++i) {
        const BrainV2Cell& c = brain.cells[i];
        out << i << ' ' << c.samples << ' ' << c.best.gapChoice << ' ' << c.best.lotMultiplier << ' ' << c.best.maxLevels << ' '
            << c.best.exitFactor << ' ' << c.meanPnl << ' ' << c.sumPnl << ' ' << c.sumSquares << ' ' << c.locks << ' '
            << c.lockRate << ' ' << c.meanLockLoss << ' ' << c.lockLossSum << ' ' << c.score << ' ' << c.firstSide << "\n";
    }
    return out.str();
}

std::optional<BrainV2> parseBrainV2(const std::string& text) {
    std::istringstream in(text);
    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "muuk-brain-v2" || version != 1) return std::nullopt;
    BrainV2 brain;
    std::string key;
    if (!(in >> key >> brain.volLow >> key >> brain.volHigh >> key >> brain.trainedBars >> key >> brain.trainedUntilMs >> key)) {
        return std::nullopt;
    }
    int forced = 0;
    if (!(in >> forced)) return std::nullopt;
    brain.forced = forced != 0;
    // Step 2: optional trailing fields on the header line -- shiftAfterFills
    // <n> stepAtr <x>. An old file ends the line after forced; the rest is
    // empty and the brain keeps shift 0 (config shift as the fallback).
    {
        std::string rest;
        std::getline(in, rest);
        std::istringstream fields(rest);
        std::string field;
        while (fields >> field) {
            if (field == "shiftAfterFills") {
                int s = 0;
                if (fields >> s) brain.shiftAfterFills = s;
            } else if (field == "stepAtr") {
                double s = 0.0;
                if (fields >> s) brain.stepAtr = s;
            } else if (field == "erMode") {
                int e = 0;
                if (fields >> e) brain.erMode = e;
            } else if (field == "stopLoss") {
                double s = 0.0;
                if (fields >> s) brain.stopLoss = s;
            } else {
                std::string skip;
                fields >> skip;  // unknown field: skip its value, stay tolerant
            }
        }
    }
    if (!(in >> key)) return std::nullopt;  // gapAtrChoices
    std::string line;
    std::getline(in, line);
    std::istringstream gaps(line);
    double g = 0.0;
    while (gaps >> g) brain.gapAtrChoices.push_back(g);
    if (brain.gapAtrChoices.empty()) return std::nullopt;
    brain.cells.assign(kCells, BrainV2Cell{});
    for (int i = 0; i < kCells; ++i) {
        // One line per cell, so the trailing firstSide (Step 3) can be
        // told apart from the next cell's index: an old file ends the
        // line after score, and the cell keeps firstSide 0 (Both /
        // the config first order as the fallback).
        std::string cellLine;
        if (!std::getline(in, cellLine)) return std::nullopt;
        std::istringstream cellIn(cellLine);
        int idx = -1;
        BrainV2Cell c;
        if (!(cellIn >> idx >> c.samples >> c.best.gapChoice >> c.best.lotMultiplier >> c.best.maxLevels >>
              c.best.exitFactor >> c.meanPnl >> c.sumPnl >> c.sumSquares >> c.locks >> c.lockRate >> c.meanLockLoss >>
              c.lockLossSum >> c.score)) {
            return std::nullopt;
        }
        if (idx < 0 || idx >= kCells) return std::nullopt;
        int firstSide = 0;
        if (cellIn >> firstSide && (firstSide == -1 || firstSide == 0 || firstSide == 1 || firstSide == 2)) {
            c.firstSide = firstSide;
        }
        brain.cells[idx] = c;
    }
    brain.trained = true;
    return brain;
}

void observeBasketOutcome(BrainV2& brain, const SituationV2& situation, const BrainV2Policy& used, double pnl, bool locked,
                          const BrainV2Config& config) {
    if (!brain.trained || brain.cells.size() != kCells || !situation.known) return;
    BrainV2Cell& cell = brain.cells[cellIndex(situation.base)];
    if (cell.best.gapChoice != used.gapChoice) return;  // only the cell/policy actually traded
    const int maxN = std::max(1, config.onlineMaxSamples);
    if (cell.samples >= maxN) {
        const double scale = static_cast<double>(maxN - 1) / maxN;
        cell.sumPnl *= scale;
        cell.sumSquares *= scale;
        cell.locks = static_cast<int>(cell.locks * scale);
        cell.lockLossSum *= scale;
        cell.samples = maxN - 1;
    }
    ++cell.samples;
    cell.sumPnl += pnl;
    cell.sumSquares += pnl * pnl;
    if (locked) {
        ++cell.locks;
        cell.lockLossSum += std::fabs(pnl);
    }
    cell.meanPnl = cell.sumPnl / cell.samples;
    const double variance = std::max(0.0, cell.sumSquares / cell.samples - cell.meanPnl * cell.meanPnl);
    cell.score = cell.meanPnl - std::sqrt(variance / cell.samples);
    cell.lockRate = static_cast<double>(cell.locks) / cell.samples;
    cell.meanLockLoss = cell.locks > 0 ? cell.lockLossSum / cell.locks : 0.0;
    const double shrunkLockRate = (cell.locks + 1.0) / (cell.samples + 10.0);
    if (cell.score <= 0.0 || shrunkLockRate > config.maxLockRate) {
        cell.best.gapChoice = -1;  // veto only: no new policy is invented online
    }
}

}  // namespace muuk::core
