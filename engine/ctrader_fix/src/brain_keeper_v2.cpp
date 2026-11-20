#include "muuk/ctrader_fix/brain_keeper_v2.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace muuk::ctrader_fix {

namespace fs = std::filesystem;
using muuk::core::Bar;

namespace {

constexpr int kNewBarsToLearn = 1440;  // a day of minutes
constexpr const char* kLiveBars = "bars_live.csv";

double headerValue(const std::string& line, const std::string& key, double fallback) {
    const auto at = line.find(key + "=");
    if (at == std::string::npos) return fallback;
    try {
        return std::stod(line.substr(at + key.size() + 1));
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

HistoryBarsV2 loadHistoryFolderV2(const fs::path& folder) {
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
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf,%lf,%lf", &time, &open, &high, &low, &close, &ticks, &spread) < 5) continue;
            if (!(high >= low) || !(low > 0.0)) continue;
            const int64_t ms = (time - static_cast<int64_t>(gmtOffset)) * 1000;
            merged[ms] = {Bar{ms, open, high, low, close, ticks}, spread * point};
        }
    }
    HistoryBarsV2 out;
    out.bars.reserve(merged.size());
    out.spreads.reserve(merged.size());
    for (const auto& [ms, bar] : merged) {
        out.bars.push_back(bar.first);
        out.spreads.push_back(bar.second);
    }
    return out;
}

std::optional<muuk::core::BrainV2> loadBrainV2File(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    std::stringstream buffer;
    buffer << in.rdbuf();
    return muuk::core::parseBrainV2(buffer.str());
}

void saveBrainV2File(const fs::path& file, const muuk::core::BrainV2& brain) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << muuk::core::serializeBrainV2(brain);
    }
    fs::rename(tmp, file);
}

BrainKeeperV2::BrainKeeperV2(fs::path brainFile, muuk::core::BrainV2Config config, Log log)
    : file_(std::move(brainFile)), config_(std::move(config)), log_(std::move(log)) {
    if (auto loaded = loadBrainV2File(file_)) {
        brain_ = std::move(*loaded);
        lastSavedBarMs_ = brain_.trainedUntilMs;
        if (log_) log_("[brain v2] loaded " + file_.string() + ", learned from " + std::to_string(brain_.trainedBars) + " bars");
    } else {
        if (log_) log_("[brain v2] no brain at " + file_.string() + " yet: learning from the bars in its folder");
        learnInBackground();
    }
}

BrainKeeperV2::~BrainKeeperV2() {
    if (thread_.joinable()) thread_.join();
}

muuk::core::BrainV2Decision BrainKeeperV2::decide(const std::deque<Bar>& oneMinuteBars, int recentOutcome, double spread,
                                                  double medianSpread, const muuk::core::BrainAccountState& account) const {
    return decideForCycle(oneMinuteBars, recentOutcome, spread, medianSpread, account).decision;
}

BrainV2Cycle BrainKeeperV2::decideForCycle(const std::deque<Bar>& oneMinuteBars, int recentOutcome, double spread,
                                           double medianSpread, const muuk::core::BrainAccountState& account) const {
    std::scoped_lock lock(mutex_);
    BrainV2Cycle out;
    out.situation = muuk::core::situationAtV2(oneMinuteBars, brain_, recentOutcome, spread, medianSpread);
    out.decision = muuk::core::decideWithBrainV2(brain_, out.situation, config_, account);
    return out;
}

bool BrainKeeperV2::trained() const {
    std::scoped_lock lock(mutex_);
    return brain_.trained;
}

std::string BrainKeeperV2::status() const {
    std::scoped_lock lock(mutex_);
    std::string out = !brain_.trained ? std::string("no v2 brain has passed its test yet")
                      : brain_.forced ? "FORCED on by hand (did not pass its test), learned from " + std::to_string(brain_.trainedBars) + " bars"
                                      : "passed its test, learned from " + std::to_string(brain_.trainedBars) + " bars";
    if (!lastTry_.empty()) out += "; last try: " + lastTry_;
    return out;
}

void BrainKeeperV2::observeBasket(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Policy& used, double pnl,
                                  bool locked) {
    std::scoped_lock lock(mutex_);
    muuk::core::observeBasketOutcome(brain_, situation, used, pnl, locked, config_);
}

void BrainKeeperV2::addBars(const std::deque<Bar>& oneMinuteBars, double spread) {
    std::error_code ec;
    if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
    const fs::path liveFile = file_.parent_path() / kLiveBars;
    const bool fresh = !fs::exists(liveFile, ec);
    std::ofstream out(liveFile, std::ios::app);
    if (fresh) out << "# bars the bot saw: UTC seconds, spread in price\n# point=1,gmtOffset=0\n";
    char line[160];
    int added = 0;
    for (const Bar& b : oneMinuteBars) {
        if (b.startMs <= lastSavedBarMs_) continue;
        std::snprintf(line, sizeof(line), "%lld,%.5f,%.5f,%.5f,%.5f,%.0f,%.5f\n", static_cast<long long>(b.startMs / 1000), b.open,
                      b.high, b.low, b.close, b.tickVolume, spread);
        out << line;
        lastSavedBarMs_ = b.startMs;
        ++added;
    }
    newBars_ += added;
    if (newBars_ >= kNewBarsToLearn) {
        newBars_ = 0;
        learnInBackground();
    }
}

void BrainKeeperV2::learnInBackground() {
    if (learning_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // never ahead of quotes and fills
#endif
        const HistoryBarsV2 history = loadHistoryFolderV2(file_.parent_path());
        muuk::core::BrainV2 current;
        {
            std::scoped_lock lock(mutex_);
            current = brain_;
        }
        muuk::core::BrainV2Training t = muuk::core::trainBrainV2(history.bars, history.spreads, current, config_);
        {
            std::ofstream report(file_.parent_path() / "training_report_v2.txt", std::ios::app);
            const std::time_t now = std::time(nullptr);
            char when[32];
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            report << "=== " << when << "\n" << t.report << "\n";
        }
        if (t.better && t.brain.trained) {
            try {
                saveBrainV2File(file_, t.brain);
                std::scoped_lock lock(mutex_);
                brain_ = t.brain;
            } catch (const std::exception& ex) {
                if (log_) log_(std::string("[brain v2] can't save: ") + ex.what());
            }
        }
        {
            char summary[140];
            std::snprintf(summary, sizeof(summary), "%s on %zu bars (v2 test %+.0f, fixed gap %+.0f, v1 %+.0f, gates %s)",
                          t.better ? "adopted" : "not better", history.bars.size(), t.brainTest.totalPnl, t.fixedTest.totalPnl,
                          t.v1Test.totalPnl, t.gatesPassed ? "passed" : "not passed");
            std::scoped_lock lock(mutex_);
            lastTry_ = summary;
        }
        if (log_) log_(std::string("[brain v2] learned again on ") + std::to_string(history.bars.size()) + " bars: " +
                       (t.better ? "better on bars it never saw, adopted" : "not better, the current one stays"));
        learning_ = false;
    });
}

}  // namespace muuk::ctrader_fix
