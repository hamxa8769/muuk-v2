#include "muuk/core/market_brain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "muuk/core/regime_range.hpp"

namespace muuk::core {

namespace {

constexpr int kCells = 81;
constexpr int kWindowBars = 240;  // what a decision sees, as live

int cellIndex(const Situation& s) { return s.volatility * 27 + s.trend * 9 + s.chop * 3 + s.session; }

int sessionOf(int64_t utcMs) {
    const int hour = static_cast<int>(((utcMs / 3'600'000) % 24 + 24) % 24);
    if (hour >= 13 && hour < 21) return 2;
    if (hour >= 7 && hour < 13) return 1;
    return 0;
}

Situation situationFrom(const std::deque<Bar>& window, double volLow, double volHigh) {
    Situation s;
    if (window.size() < 150) return s;  // 30 five-minute bars
    const std::deque<Bar> five = aggregateBars(window, 5);
    const double adx = adxOf(five, 14);
    const double chop = choppinessOf(five, 14);
    s.atr = atrOf(window, 14);
    s.volatility = s.atr < volLow ? 0 : (s.atr < volHigh ? 1 : 2);
    s.trend = adx < 20.0 ? 0 : (adx < 30.0 ? 1 : 2);
    s.chop = chop < 45.0 ? 0 : (chop < 60.0 ? 1 : 2);
    s.session = sessionOf(window.back().startMs);
    s.known = s.atr > 0.0;
    return s;
}

std::deque<Bar> windowEndingAt(const std::vector<Bar>& bars, int last) {
    const int first = std::max(0, last - kWindowBars + 1);
    return std::deque<Bar>(bars.begin() + first, bars.begin() + last + 1);
}

struct Tally {
    int n = 0;
    double sum = 0.0;
    double squares = 0.0;
};

// Terciles and the table, from bars[0 .. end). The score is the mean less one
// standard error, so a lucky handful doesn't win.
MarketBrain learn(const std::vector<Bar>& bars, const std::vector<double>& spreads, int end, const BrainConfig& config) {
    MarketBrain brain;
    brain.gapAtrChoices = config.gapAtrChoices;
    std::vector<double> atrs;
    for (int i = kWindowBars; i < end; i += config.stepBars) atrs.push_back(atrOf(windowEndingAt(bars, i), 14));
    if (atrs.size() < 100) return brain;
    std::sort(atrs.begin(), atrs.end());
    brain.volLow = atrs[atrs.size() / 3];
    brain.volHigh = atrs[atrs.size() * 2 / 3];

    const std::size_t choices = config.gapAtrChoices.size();
    std::vector<Tally> tally(kCells * choices);
    for (int i = kWindowBars; i + 1 < end; i += config.stepBars) {
        const Situation s = situationFrom(windowEndingAt(bars, i), brain.volLow, brain.volHigh);
        if (!s.known) continue;
        BrainConfig bounded = config;
        bounded.maxCycleBars = std::min(config.maxCycleBars, end - 1 - i);  // nothing from past the end
        for (std::size_t c = 0; c < choices; ++c) {
            const double gap = clampedGap(config.gapAtrChoices[c], s.atr, config);
            const ReplayCycle r = replayCycle(bars, spreads, i, gap, s.atr, bounded);
            Tally& t = tally[cellIndex(s) * choices + c];
            ++t.n;
            t.sum += r.pnl;
            t.squares += r.pnl * r.pnl;
        }
    }
    brain.cells.assign(kCells, BrainCell{});
    for (int cell = 0; cell < kCells; ++cell) {
        double bestScore = 0.0;  // only something better than not trading
        for (std::size_t c = 0; c < choices; ++c) {
            const Tally& t = tally[cell * choices + c];
            brain.cells[cell].samples = std::max(brain.cells[cell].samples, t.n);
            if (t.n < config.minSamples) continue;
            const double mean = t.sum / t.n;
            const double variance = std::max(0.0, t.squares / t.n - mean * mean);
            const double score = mean - std::sqrt(variance / t.n);
            if (score > bestScore) {
                bestScore = score;
                brain.cells[cell].bestChoice = static_cast<int>(c);
                brain.cells[cell].meanPnl = mean;
            }
        }
    }
    brain.trained = true;
    brain.trainedBars = end;
    brain.trainedUntilMs = bars[end - 1].startMs;
    return brain;
}

std::string lineOf(const char* name, const ReplayResult& r) {
    char buf[220];
    std::snprintf(buf, sizeof(buf), "%-14s cycles %5d  wins %5d  locks %4d  total %10.2f  worst cycle %9.2f  max drawdown %9.2f\n",
                  name, r.cycles, r.wins, r.locks, r.totalPnl, r.worstCycle, r.maxDrawdown);
    return buf;
}

}  // namespace

ReplayResult replayRules(const std::vector<Bar>& bars, const std::vector<double>& spreads, int from, const MarketBrain* brain,
                         double fixedGapAtr, const BrainConfig& config) {
    ReplayResult out;
    double equity = 0.0;
    double peak = 0.0;
    const int last = static_cast<int>(bars.size()) - 1;
    for (int i = from; i < last;) {
        const std::deque<Bar> window = windowEndingAt(bars, i);
        double gap = 0.0;
        double atr = 0.0;
        if (fixedGapAtr > 0.0) {
            atr = atrOf(window, 14);
            gap = atr > 0.0 ? clampedGap(fixedGapAtr, atr, config) : 0.0;
        } else if (brain) {
            const Situation s = situationAt(window, *brain);
            const BrainDecision d = decideWithBrain(*brain, s, config);
            if (d.trade) {
                gap = d.gap;
                atr = s.atr;
            }
        }
        if (gap <= 0.0) {
            i += config.stepBars;
            continue;
        }
        BrainConfig bounded = config;
        bounded.maxCycleBars = std::min(config.maxCycleBars, last - i);
        const ReplayCycle r = replayCycle(bars, spreads, i, gap, atr, bounded);
        ++out.cycles;
        if (r.pnl > 0.0) ++out.wins;
        if (r.locked) ++out.locks;
        out.totalPnl += r.pnl;
        out.worstCycle = std::min(out.worstCycle, r.pnl);
        equity += r.pnl;
        peak = std::max(peak, equity);
        out.maxDrawdown = std::max(out.maxDrawdown, peak - equity);
        i = std::max(r.endBar, i + 1);
    }
    return out;
}

MarketBrain learnBrain(const std::vector<Bar>& bars, const std::vector<double>& spreads, int end, const BrainConfig& config) {
    return learn(bars, spreads, end, config);
}

double clampedGap(double gapAtr, double atr, const BrainConfig& config) {
    return std::clamp(gapAtr * atr, config.minGap, config.maxGap);
}

Situation situationAt(const std::deque<Bar>& oneMinuteBars, const MarketBrain& brain) {
    return situationFrom(oneMinuteBars, brain.volLow, brain.volHigh);
}

BrainDecision decideWithBrain(const MarketBrain& brain, const Situation& s, const BrainConfig& config) {
    BrainDecision d;
    if (!brain.trained || brain.cells.size() != kCells) {
        d.why = "the brain isn't trained";
        return d;
    }
    if (!s.known) {
        d.why = "too few bars to read this market";
        return d;
    }
    static constexpr const char* kLevels[] = {"low", "mid", "high"};
    static constexpr const char* kSessions[] = {"Asia", "London", "New York"};
    char where[120];
    std::snprintf(where, sizeof(where), "volatility %s, trend %s, chop %s, %s", kLevels[s.volatility], kLevels[s.trend],
                  kLevels[s.chop], kSessions[s.session]);
    const BrainCell& cell = brain.cells[cellIndex(s)];
    const int choice = cell.bestChoice;
    if (choice < 0 || choice >= static_cast<int>(brain.gapAtrChoices.size())) {
        char buf[220];
        std::snprintf(buf, sizeof(buf), "%s: history shows no gap worth trading here (%d samples)", where, cell.samples);
        d.why = buf;
        return d;
    }
    d.trade = true;
    d.gap = clampedGap(brain.gapAtrChoices[choice], s.atr, config);
    char buf[240];
    std::snprintf(buf, sizeof(buf), "%s: gap %.2f (%.1f x ATR %.2f), %d samples, mean %+.2f per cycle", where, d.gap,
                  brain.gapAtrChoices[choice], s.atr, cell.samples, cell.meanPnl);
    d.why = buf;
    return d;
}

ReplayCycle replayCycle(const std::vector<Bar>& bars, const std::vector<double>& spreads, int start, double gap, double atr,
                        const BrainConfig& config) {
    struct Leg {
        bool buy;
        double price;
        double lot;
    };
    ReplayCycle out;
    const double mid = bars[start].close;
    double buyLine = mid + gap / 2.0;
    double sellLine = mid - gap / 2.0;
    std::vector<Leg> legs;
    double costs = 0.0;
    bool nextBuy = true;  // once something filled: the opposite side only

    auto netAt = [&](double price) {
        double net = -costs;
        for (const Leg& l : legs) net += (l.buy ? price - l.price : l.price - price) * l.lot;
        return net;
    };
    const int end = std::min(static_cast<int>(bars.size()) - 1, start + config.maxCycleBars);
    for (int j = start + 1; j <= end; ++j) {
        const Bar& bar = bars[j];
        const double spread = static_cast<std::size_t>(j) < spreads.size() ? spreads[j] : 0.0;
        // The inside of a bar isn't known, so this takes the harder reading:
        // fills first (two at most: one per line), the target only on a bar without any.
        int fillsThisBar = 0;
        while (fillsThisBar < 2) {
            const bool buyHit = (legs.empty() || nextBuy) && bar.high >= buyLine;
            const bool sellHit = (legs.empty() || !nextBuy) && bar.low <= sellLine;
            if (!buyHit && !sellHit) break;
            const bool buy = buyHit && (!sellHit || bar.close >= mid);
            if (static_cast<int>(legs.size()) >= config.maxPositions) {
                // The next level is the lock: the loss freezes at its line.
                const double line = buy ? buyLine : sellLine;
                out.pnl = netAt(line) - spread * std::fabs(netAt(1.0) - netAt(0.0));
                out.endBar = j;
                out.fills = static_cast<int>(legs.size());
                out.locked = true;
                return out;
            }
            const double lot = std::pow(config.lotMultiplier, static_cast<double>(legs.size()));
            legs.push_back(Leg{buy, buy ? buyLine : sellLine, lot});
            costs += spread * lot;
            nextBuy = !buy;
            ++fillsThisBar;
            if (config.shiftAfterFills > 0 && static_cast<int>(legs.size()) % config.shiftAfterFills == 0) {
                const double gapNow = buyLine - sellLine;
                const double wider = config.stepAtr > 0.0 ? std::min(config.maxGap, std::max(gapNow + config.stepAtr * atr, gapNow + 1.0))
                                                          : gapNow;
                const double center = (buyLine + sellLine) / 2.0;
                buyLine = center + wider / 2.0;
                sellLine = center - wider / 2.0;
            }
        }
        if (legs.empty()) continue;
        // The last-lot target, on half the gap the cycle started with (as live).
        const double target = legs.back().lot * gap / 2.0;
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

WalkForwardResult walkForward(const std::vector<Bar>& bars, const std::vector<double>& spreads, const MarketBrain* brain,
                              bool learnEachPeriod, const BrainConfig& config) {
    WalkForwardResult out;
    const int total = static_cast<int>(bars.size());
    const int first = std::max(kWindowBars * 2, static_cast<int>(total * config.firstLearnShare));
    const int length = std::max(kWindowBars * 2, (total - first) / std::max(1, config.periods));
    double equity = 0.0;
    double peak = 0.0;
    for (int from = first; from + kWindowBars < total; from += length) {
        const int to = std::min(total, from + length);
        const std::vector<Bar> upTo(bars.begin(), bars.begin() + to);
        const std::vector<double> spreadsUpTo(spreads.begin(), spreads.begin() + std::min<std::size_t>(spreads.size(), to));
        ReplayResult r;
        if (learnEachPeriod) {
            const MarketBrain learned = learn(bars, spreads, from, config);
            if (!learned.trained) continue;
            r = replayRules(upTo, spreadsUpTo, from, &learned, 0.0, config);
        } else {
            r = replayRules(upTo, spreadsUpTo, from, brain, brain ? 0.0 : 3.0, config);
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

BrainTraining trainBrain(const std::vector<Bar>& bars, const std::vector<double>& spreads, const MarketBrain& current,
                         const BrainConfig& config) {
    BrainTraining t;
    const int total = static_cast<int>(bars.size());
    const int first = static_cast<int>(total * config.firstLearnShare);
    if (first < kWindowBars * 2 || total - first < kWindowBars * 2) {
        t.report = "too few bars to learn from: " + std::to_string(total) + "\n";
        return t;
    }
    t.tested = learn(bars, spreads, first, config);
    if (!t.tested.trained) {
        t.report = "too few bars to learn from: " + std::to_string(total) + "\n";
        return t;
    }
    const WalkForwardResult brain = walkForward(bars, spreads, nullptr, true, config);
    const WalkForwardResult fixed = walkForward(bars, spreads, nullptr, false, config);
    t.brainTest = brain.total;
    t.fixedTest = fixed.total;
    t.periods = brain.periods;
    t.brainPeriodsUp = brain.periodsUp;
    if (current.trained) t.currentTest = walkForward(bars, spreads, &current, false, config).total;
    t.better = t.brainTest.cycles > 0 && t.brainTest.totalPnl > 0.0 && t.brainTest.totalPnl > t.fixedTest.totalPnl &&
               t.brainTest.maxDrawdown <= t.fixedTest.maxDrawdown &&
               (!current.trained || t.brainTest.totalPnl > t.currentTest.totalPnl);
    if (t.better) t.brain = learn(bars, spreads, total, config);

    char head[200];
    std::snprintf(head, sizeof(head), "bars %d, walking forward over %d periods after the first %d (per 0.01 lot start); brain up in %d\n",
                  total, brain.periods, first, brain.periodsUp);
    t.report = head + lineOf("brain", t.brainTest) + lineOf("fixed 3 x ATR", t.fixedTest);
    if (current.trained) t.report += lineOf("brain in use", t.currentTest);
    t.report += t.better ? "result: better walking forward -- adopted\n" : "result: not better -- not adopted\n";
    return t;
}

}  // namespace muuk::core
