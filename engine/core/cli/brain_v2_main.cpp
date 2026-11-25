// Muuk-Testing-BrainV2: trains Brain v2 on a folder of one-minute history
// (MT5 export format, as in BrainKeeper::loadHistoryFolder) and prints the
// walk-forward table for v2 vs fixed 3 x ATR vs the v1 brain, plus the
// held-out last 15% and the gate verdict. Demo-only: a brain is saved only
// when it passes the STRICT gates (better + drawdown + locks + periods up +
// held-out positive), not merely the walk-forward adoption test -- the
// 2026-11-20 run saved a gates-failed brain that way.
//
//   muuk_brain_v2_trainer <history folder> [--bars N] [--slippage X]
//       [--commission X] [--periods N] [--er-mode N] [--first-side <learn|both|buy|sell|trend>]
//       [--shift-after-fills N] [--step-atr X] [--shift-search] [--stop-loss X] [--out brain_v2.txt]
//   --stop-loss (Step 5): the live basket stop loss in replay units --
//   the replay closes a basket at -X instead of running on to the lock.
//   0 (default) trains exactly as the shipped brain was trained; the
//   in-app trainer passes the panel's stop loss (50) here.
//   --first-side: how the first order is chosen (Step 3). learn = one
//   per cell, from that cell's history (default); both = both stops at
//   once (the shipped behaviour, reproduces the Step 2 numbers); buy /
//   sell / trend = one side forced everywhere (experiment controls).
//   --er-mode: 0 = off (shipped behaviour), 1 = block choppy cycles
//   (ER < 0.3), 2 = 1 + a clean trend (ER >= 0.5) overrides the strong-trend
//   block and the ATR breaker, 3 = 2 + the ATR percentile replaces the
//   fixed ATR breaker (block when atrPct > 90).
//   --shift-after-fills / --step-atr: the shift the replay trains with
//   (Step 2, decision D12); the winning shift is written into the brain
//   file, so the live runner trades the validated shift. Defaults 3 / 2.0.
//   --shift-search: try shiftAfterFills {2,3,4} x stepAtr {1.0,2.0} and
//   save the best gates-passing combo (fewest held-out locks first);
//   when none passes, nothing is saved and the shipped shift stays.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "muuk/core/market_brain_v2.hpp"

namespace fs = std::filesystem;
using namespace muuk::core;

namespace {

double headerValue(const std::string& line, const std::string& key, double fallback) {
    const auto at = line.find(key + "=");
    if (at == std::string::npos) return fallback;
    try {
        return std::stod(line.substr(at + key.size() + 1));
    } catch (const std::exception&) {
        return fallback;
    }
}

const char* firstSideModeName(BrainFirstSideMode mode) {
    switch (mode) {
        case BrainFirstSideMode::Learn: return "learn";
        case BrainFirstSideMode::Both: return "both";
        case BrainFirstSideMode::BuyFirst: return "buy";
        case BrainFirstSideMode::SellFirst: return "sell";
        case BrainFirstSideMode::TrendFirst: return "trend";
    }
    return "learn";
}

// Step 3: what the brain learned for the first order -- the split over
// its tradable cells, so the experiment's verdict is visible even when
// the gates fail and nothing is saved.
void printFirstSideSplit(const BrainV2& brain, const char* label) {
    if (!brain.trained) return;
    int both = 0, buy = 0, sell = 0, trend = 0, tradable = 0;
    std::string buyCells, sellCells, trendCells;
    for (std::size_t i = 0; i < brain.cells.size(); ++i) {
        const BrainV2Cell& c = brain.cells[i];
        if (c.best.gapChoice < 0) continue;
        ++tradable;
        if (c.firstSide == 1) {
            ++buy;
            buyCells += std::to_string(i) + " ";
        } else if (c.firstSide == -1) {
            ++sell;
            sellCells += std::to_string(i) + " ";
        } else if (c.firstSide == 2) {
            ++trend;
            trendCells += std::to_string(i) + " ";
        } else {
            ++both;
        }
    }
    std::cout << "first side (" << label << "): " << tradable << " tradable cells: Both " << both << ", Buy-first " << buy;
    if (!buyCells.empty()) std::cout << " [" << buyCells << "]";
    std::cout << ", Sell-first " << sell;
    if (!sellCells.empty()) std::cout << " [" << sellCells << "]";
    std::cout << ", trend-first " << trend;
    if (!trendCells.empty()) std::cout << " [" << trendCells << "]";
    std::cout << "\n";
}

struct History {
    std::vector<Bar> bars;
    std::vector<double> spreads;
};

History loadHistoryFolder(const fs::path& folder) {
    std::map<int64_t, std::pair<Bar, double>> merged;
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_regular_file(folder, ec)) files.push_back(folder);
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().extension() == ".csv") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::ifstream in(file);
        std::string line;
        double point = 0.01;
        double gmtOffset = 0.0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (line[0] == '#') {
                point = headerValue(line, "point", point);
                gmtOffset = headerValue(line, "gmtOffset", gmtOffset);
                continue;
            }
            long long time = 0;
            double open = 0.0, high = 0.0, low = 0.0, close = 0.0, ticks = 0.0, spread = 0.0;
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf,%lf,%lf", &time, &open, &high, &low, &close, &ticks, &spread) < 5)
                continue;
            if (!(high >= low) || !(low > 0.0)) continue;
            const int64_t ms = (time - static_cast<int64_t>(gmtOffset)) * 1000;
            merged[ms] = {Bar{ms, open, high, low, close, ticks}, spread * point};
        }
    }
    History out;
    out.bars.reserve(merged.size());
    out.spreads.reserve(merged.size());
    for (const auto& [ms, bar] : merged) {
        out.bars.push_back(bar.first);
        out.spreads.push_back(bar.second);
    }
    return out;
}

// Step 2 shift search: one full walk-forward train per combo, then the
// best gates-passing combo (fewest held-out locks first, see
// pickBestShiftCombo) is the one saved. Nothing is saved when no combo
// passes -- the shipped shift stays, and the report says so plainly.
int shiftSearchMain(const History& history, BrainV2Config cfg, const std::string& outFile) {
    const int fillsGrid[] = {2, 3, 4};
    const double stepGrid[] = {1.0, 2.0};
    std::vector<ShiftSearchCandidate> candidates;
    std::vector<BrainV2Training> trainings;
    for (const int fills : fillsGrid) {
        for (const double step : stepGrid) {
            cfg.base.shiftAfterFills = fills;
            cfg.base.stepAtr = step;
            const BrainV2Training t = trainBrainV2(history.bars, history.spreads, BrainV2{}, cfg);
            candidates.push_back(ShiftSearchCandidate{fills, step, t.gatesPassed, t.brainTest.totalPnl, t.brainTest.locks,
                                                      t.heldOutV2.totalPnl, t.heldOutV2.locks, t.brainPeriodsUp});
            trainings.push_back(t);
            std::cout << "shift " << fills << "x" << step << ": cycles " << t.brainTest.cycles << " wins "
                      << t.brainTest.wins << " locks " << t.brainTest.locks << " total " << t.brainTest.totalPnl
                      << " max drawdown " << t.brainTest.maxDrawdown << " up " << t.brainPeriodsUp << " | held-out "
                      << t.heldOutV2.totalPnl << " / " << t.heldOutV2.locks << " locks | gates "
                      << (t.gatesPassed ? "PASSED" : "NOT PASSED") << "\n";
        }
    }
    const std::optional<ShiftSearchCandidate> best = pickBestShiftCombo(candidates);
    const ShiftSearchCandidate* bestOverall = nullptr;
    for (const ShiftSearchCandidate& c : candidates) {
        if (bestOverall == nullptr || shiftComboBetter(c, *bestOverall)) bestOverall = &c;
    }
    if (!best.has_value()) {
        std::cout << "shift search: no combo passed the strict gates; best overall " << bestOverall->shiftAfterFills
                  << "x" << bestOverall->stepAtr << " (held-out " << bestOverall->heldOutPnl << " / "
                  << bestOverall->heldOutLocks << " locks) -- nothing saved, the shipped shift stays\n";
        return 1;
    }
    std::size_t winner = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].shiftAfterFills == best->shiftAfterFills && candidates[i].stepAtr == best->stepAtr) winner = i;
    }
    const BrainV2Training& t = trainings[winner];
    std::cout << "shift search: best gates-passing " << best->shiftAfterFills << "x" << best->stepAtr
              << " (held-out " << best->heldOutPnl << " / " << best->heldOutLocks << " locks, total " << best->totalPnl
              << ", " << best->locks << " locks)\n";
    if (t.brain.trained) {
        int tradable = 0;
        for (const auto& c : t.brain.cells) {
            if (c.best.gapChoice >= 0) ++tradable;
        }
        std::cout << "tradable cells: " << tradable << " of " << t.brain.cells.size() << "\n";
        std::ofstream out(outFile, std::ios::trunc);
        out << serializeBrainV2(t.brain);
        std::cout << "saved " << outFile << " (strict gates PASSED, shift " << best->shiftAfterFills << "x"
                  << best->stepAtr << ")\n";
        return 0;
    }
    std::cout << "nothing saved: the winning combo has no trained brain -- see above\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: muuk_brain_v2_trainer <history folder or csv> [--bars N] [--slippage X] "
                     "[--commission X] [--periods N] [--er-mode N] [--out brain_v2.txt]\n";
        return 2;
    }
    BrainV2Config cfg;
    int maxBars = 0;
    bool shiftSearch = false;
    std::string outFile = "brain_v2.txt";
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--shift-search") {
            shiftSearch = true;
            continue;
        }
        if (i + 1 >= argc) break;
        const std::string value = argv[++i];  // consume the flag's value, so it is never re-read as a key
        if (key == "--bars") maxBars = std::stoi(value);
        if (key == "--slippage") cfg.slippage = std::stod(value);
        if (key == "--commission") cfg.commission = std::stod(value);
        if (key == "--periods") cfg.base.periods = std::stoi(value);
        if (key == "--er-mode") cfg.erMode = std::stoi(value);
        if (key == "--first-side") {
            if (value == "learn") cfg.firstSideMode = BrainFirstSideMode::Learn;
            else if (value == "both") cfg.firstSideMode = BrainFirstSideMode::Both;
            else if (value == "buy") cfg.firstSideMode = BrainFirstSideMode::BuyFirst;
            else if (value == "sell") cfg.firstSideMode = BrainFirstSideMode::SellFirst;
            else if (value == "trend") cfg.firstSideMode = BrainFirstSideMode::TrendFirst;
            else {
                std::cerr << "unknown --first-side value: " << value << " (learn|both|buy|sell|trend)\n";
                return 2;
            }
        }
        if (key == "--shift-after-fills") cfg.base.shiftAfterFills = std::stoi(value);
        if (key == "--step-atr") cfg.base.stepAtr = std::stod(value);
        if (key == "--stop-loss") cfg.stopLoss = std::stod(value);
        if (key == "--out") outFile = value;
    }
    History history = loadHistoryFolder(argv[1]);
    if (maxBars > 0 && static_cast<int>(history.bars.size()) > maxBars) {
        history.bars.resize(maxBars);
        history.spreads.resize(maxBars);
    }
    std::cout << "bars " << history.bars.size() << ", slippage " << cfg.slippage << ", commission " << cfg.commission
              << ", periods " << cfg.base.periods << ", er-mode " << cfg.erMode << ", shift " << cfg.base.shiftAfterFills
              << "x" << cfg.base.stepAtr << ", stop-loss " << cfg.stopLoss << ", first-side "
              << firstSideModeName(cfg.firstSideMode) << "\n";
    if (history.bars.size() < 2000) {
        std::cerr << "too few bars to learn from: " << history.bars.size() << "\n";
        return 1;
    }
    if (shiftSearch) return shiftSearchMain(history, cfg, outFile);
    const BrainV2Training t = trainBrainV2(history.bars, history.spreads, BrainV2{}, cfg);
    std::cout << t.report;
    if (t.brain.trained) {
        printFirstSideSplit(t.brain, "adopted brain, all bars");
    } else {
        // Not adopted: learn once more on all the bars just for the
        // split, so the experiment still reports what the cells learned.
        const BrainV2 diagnostic = learnBrainV2(history.bars, history.spreads, static_cast<int>(history.bars.size()), cfg);
        printFirstSideSplit(diagnostic, "diagnostic brain, all bars (not adopted)");
    }
    if (t.brain.trained && t.gatesPassed) {
        int tradable = 0;
        std::map<std::string, int> policies;
        for (const auto& c : t.brain.cells) {
            if (c.best.gapChoice < 0) continue;
            ++tradable;
            char key[64];
            std::snprintf(key, sizeof(key), "%.2fx/%d/exit%.2f", c.best.lotMultiplier, c.best.maxLevels, c.best.exitFactor);
            ++policies[key];
        }
        std::cout << "tradable cells: " << tradable << " of " << t.brain.cells.size() << "\n";
        for (const auto& [policy, count] : policies) std::cout << "  " << policy << ": " << count << " cells\n";
        std::ofstream out(outFile, std::ios::trunc);
        out << serializeBrainV2(t.brain);
        std::cout << "saved " << outFile << " (strict gates PASSED)\n";
        return 0;
    }
    if (t.better) {
        std::cout << "nothing saved: v2 passed walk-forward but NOT the strict gates (locks / held-out) -- see above\n";
    } else {
        std::cout << "nothing saved: v2 did not pass its test on these bars\n";
    }
    return 1;
}
