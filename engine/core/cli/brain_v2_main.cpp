// Muuk-Testing-BrainV2: trains Brain v2 on a folder of one-minute history
// (MT5 export format, as in BrainKeeper::loadHistoryFolder) and prints the
// walk-forward table for v2 vs fixed 3 x ATR vs the v1 brain, plus the
// held-out last 15% and the gate verdict. Demo-only: a brain is saved only
// when it passes the STRICT gates (better + drawdown + locks + periods up +
// held-out positive), not merely the walk-forward adoption test -- the
// 2026-11-20 run saved a gates-failed brain that way.
//
//   muuk_brain_v2_trainer <history folder> [--bars N] [--slippage X]
//       [--commission X] [--periods N] [--out brain_v2.txt]
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: muuk_brain_v2_trainer <history folder or csv> [--bars N] [--slippage X] "
                     "[--commission X] [--periods N] [--out brain_v2.txt]\n";
        return 2;
    }
    BrainV2Config cfg;
    int maxBars = 0;
    std::string outFile = "brain_v2.txt";
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--bars") maxBars = std::stoi(value);
        if (key == "--slippage") cfg.slippage = std::stod(value);
        if (key == "--commission") cfg.commission = std::stod(value);
        if (key == "--periods") cfg.base.periods = std::stoi(value);
        if (key == "--out") outFile = value;
    }
    History history = loadHistoryFolder(argv[1]);
    if (maxBars > 0 && static_cast<int>(history.bars.size()) > maxBars) {
        history.bars.resize(maxBars);
        history.spreads.resize(maxBars);
    }
    std::cout << "bars " << history.bars.size() << ", slippage " << cfg.slippage << ", commission " << cfg.commission
              << ", periods " << cfg.base.periods << "\n";
    if (history.bars.size() < 2000) {
        std::cerr << "too few bars to learn from: " << history.bars.size() << "\n";
        return 1;
    }
    const BrainV2Training t = trainBrainV2(history.bars, history.spreads, BrainV2{}, cfg);
    std::cout << t.report;
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
