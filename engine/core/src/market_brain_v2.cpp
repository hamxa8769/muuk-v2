#include "muuk/core/market_brain_v2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

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
    return out;
}

BrainV2Decision applyRiskCaps(BrainV2Decision decision, const BrainV2Config& config, const BrainAccountState& account) {
    const BrainRiskCaps& caps = config.caps;
    if (!decision.trade) return decision;
    auto refuse = [&](const char* reason) {
        decision.trade = false;
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
        while (estimatedLockCost(decision.gap, p) > lockCap && p.maxLevels > 2) {
            --p.maxLevels;
            downgraded = true;
        }
        if (estimatedLockCost(decision.gap, p) > lockCap && p.lotMultiplier > 1.0) {
            p.lotMultiplier = 1.0;
            downgraded = true;
        }
        if (estimatedLockCost(decision.gap, p) > lockCap) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "lock cost %.2f over the cap %.2f even at 1.0x / 2 levels",
                          estimatedLockCost(decision.gap, p), lockCap);
            return refuse(buf);
        }
    }
    if (downgraded) {
        decision.policy = p;
        decision.estimatedLockCost = estimatedLockCost(decision.gap, p);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "; caps downgraded the policy to %.2fx / %d levels (lock cost %.2f)", p.lotMultiplier,
                      p.maxLevels, decision.estimatedLockCost);
        decision.why += buf;
    }
    return decision;
}

BrainV2Decision decideWithBrainV2(const BrainV2& brain, const SituationV2& situation, const BrainV2Config& config,
                                   const BrainAccountState& account) {
    BrainV2Decision d;
    if (!brain.trained || brain.cells.size() != kCells) {
        d.why = "the v2 brain isn't trained";
        return d;
    }
    if (!situation.known) {
        d.why = "too few bars to read this market";
        return d;
    }
    if (config.maxAtrVolMultiple > 0.0 && brain.volHigh > 0.0 && situation.base.atr > config.maxAtrVolMultiple * brain.volHigh) {
        char buf[300];
        std::snprintf(buf, sizeof(buf), "ATR %.2f is %.1fx the training high-vol threshold %.2f -- extreme volatility, no new cycle",
                      situation.base.atr, situation.base.atr / brain.volHigh, brain.volHigh);
        d.why = buf;
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
        return d;
    }
    // Context gates: the extra features v2 sees.
    if (config.noTradeInStrongTrend && situation.base.trend == 2 && situation.trendDir != 0) {
        d.why = std::string(where) + ": strong trend, no new range cycle";
        return d;
    }
    if (situation.recentOutcome < 0 && cell.score < config.recentLossScore) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: last basket lost or locked, and this state's score is only %.2f", where, cell.score);
        d.why = buf;
        return d;
    }
    if (situation.volume == 2 && cell.score < config.highVolumeScore) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: tick-volume spike, and this state's score is only %.2f", where, cell.score);
        d.why = buf;
        return d;
    }
    if (config.maxSpreadMultiple > 0.0 && situation.spreadPct > config.maxSpreadMultiple) {
        char buf[400];
        std::snprintf(buf, sizeof(buf), "%s: spread %.1fx its median, too wide to trade", where, situation.spreadPct);
        d.why = buf;
        return d;
    }
    d.trade = true;
    d.policy = cell.best;
    d.gap = clampedGap(brain.gapAtrChoices[cell.best.gapChoice], situation.base.atr, config.base);
    d.lockRisk = cell.lockRate;
    d.estimatedLockCost = estimatedLockCost(d.gap, d.policy);
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s: gap %.2f (%.1f x ATR %.2f), %.2fx / %d levels, exit x%.2f, %d samples, mean %+.2f, lock rate %.1f%%, "
                  "lock cost %.2f",
                  where, d.gap, brain.gapAtrChoices[cell.best.gapChoice], situation.base.atr, d.policy.lotMultiplier,
                  d.policy.maxLevels, d.policy.exitFactor, cell.samples, cell.meanPnl, 100.0 * cell.lockRate,
                  d.estimatedLockCost);
    d.why = buf;
    return applyRiskCaps(d, config, account);
}

ReplayCycleV2 replayCycleV2(const std::vector<Bar>& bars, const std::vector<double>& spreads, int start, double gap, double atr,
                             const BrainV2Policy& policy, const BrainV2Config& config) {
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
            const bool buyHit = (legs.empty() || nextBuy) && bar.high >= buyLine;
            const bool sellHit = (legs.empty() || !nextBuy) && bar.low <= sellLine;
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
    brain.trained = true;
    brain.trainedBars = end;
    brain.trainedUntilMs = bars[end - 1].startMs;
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
            }
        }
        if (gap <= 0.0) {
            i += config.base.stepBars;
            recentOutcome = 0;  // the cooldown is one decision, not the rest of the period
            continue;
        }
        BrainV2Config bounded = config;
        bounded.base.maxCycleBars = std::min(config.base.maxCycleBars, last - i);
        const ReplayCycleV2 r = replayCycleV2(bars, spreads, i, gap, atr, policy, bounded);
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
                                    bool learnEachPeriod, const BrainV2Config& config) {
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
        out.periodsUp += r.totalPnl > 0.0 ? 1 : 0;
        out.total.cycles += r.cycles;
        out.total.wins += r.wins;
        out.total.locks += r.locks;
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
                              const BrainV2Config& config) {
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
    const WalkForwardResult v2 = walkForwardV2Impl(bars, spreads, nullptr, true, config);
    const WalkForwardResult fixed = walkForwardV2Impl(bars, spreads, nullptr, false, config);
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

    t.better = t.brainTest.cycles > 0 && t.brainTest.totalPnl > 0.0 && t.brainTest.totalPnl > t.fixedTest.totalPnl &&
               t.brainTest.totalPnl > t.v1Test.totalPnl && t.brainTest.maxDrawdown <= t.fixedTest.maxDrawdown;
    const bool ddGate = t.v1Test.maxDrawdown <= 0.0 ? t.brainTest.maxDrawdown <= 0.0
                                                    : t.brainTest.maxDrawdown <= 0.5 * t.v1Test.maxDrawdown;
    const bool lockGate = t.v1Test.locks <= 0 ? t.brainTest.locks <= 0 : t.brainTest.locks <= t.v1Test.locks / 2;
    t.gatesPassed = t.better && ddGate && lockGate && t.brainPeriodsUp >= 5 && t.heldOutV2.totalPnl > 0.0;
    if (t.better) t.brain = learnBrainV2(bars, spreads, total, config);

    char head[240];
    std::snprintf(head, sizeof(head),
                  "bars %d, walking forward over %d periods after the first %d (per 0.01 lot start); "
                  "v2 up in %d; v1 numbers are without slippage/commission, so v2-vs-v1 is conservative for v2\n",
                  total, v2.periods, first, v2.periodsUp);
    t.report = head + lineOf("v2 brain", t.brainTest) + lineOf("fixed 3 x ATR", t.fixedTest) + lineOf("v1 brain", t.v1Test) +
               lineOf("held-out v2", t.heldOutV2) + lineOf("held-out fixed", t.heldOutFixed) + lineOf("held-out v1", t.heldOutV1);
    t.report += t.better ? "result: better walking forward -- adopted\n" : "result: not better -- not adopted\n";
    t.report += t.gatesPassed ? "gates: PASSED (drawdown <= 50% of v1, locks <= 50% of v1, >=5 periods up, held-out > 0)\n"
                              : "gates: NOT PASSED\n";
    (void)current;
    return t;
}

std::string serializeBrainV2(const BrainV2& brain) {
    std::ostringstream out;
    out << "muuk-brain-v2 1\n";
    out << "volLow " << brain.volLow << " volHigh " << brain.volHigh << " trainedBars " << brain.trainedBars << " trainedUntilMs "
        << brain.trainedUntilMs << " forced " << (brain.forced ? 1 : 0) << "\n";
    out << "gapAtrChoices";
    for (const double g : brain.gapAtrChoices) out << ' ' << g;
    out << "\n";
    for (std::size_t i = 0; i < brain.cells.size(); ++i) {
        const BrainV2Cell& c = brain.cells[i];
        out << i << ' ' << c.samples << ' ' << c.best.gapChoice << ' ' << c.best.lotMultiplier << ' ' << c.best.maxLevels << ' '
            << c.best.exitFactor << ' ' << c.meanPnl << ' ' << c.sumPnl << ' ' << c.sumSquares << ' ' << c.locks << ' '
            << c.lockRate << ' ' << c.meanLockLoss << ' ' << c.lockLossSum << ' ' << c.score << "\n";
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
    if (!(in >> key)) return std::nullopt;  // gapAtrChoices
    std::string line;
    std::getline(in, line);
    std::istringstream gaps(line);
    double g = 0.0;
    while (gaps >> g) brain.gapAtrChoices.push_back(g);
    if (brain.gapAtrChoices.empty()) return std::nullopt;
    brain.cells.assign(kCells, BrainV2Cell{});
    for (int i = 0; i < kCells; ++i) {
        int idx = -1;
        BrainV2Cell c;
        if (!(in >> idx >> c.samples >> c.best.gapChoice >> c.best.lotMultiplier >> c.best.maxLevels >> c.best.exitFactor >>
              c.meanPnl >> c.sumPnl >> c.sumSquares >> c.locks >> c.lockRate >> c.meanLockLoss >> c.lockLossSum >> c.score)) {
            return std::nullopt;
        }
        if (idx < 0 || idx >= kCells) return std::nullopt;
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
