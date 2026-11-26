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

// True when the folder holds any history CSV at all: a fresh install has
// none, and starting a training thread on nothing would only race the
// manual refresh for a "too few bars" result nobody asked for. Bars that
// arrive later start the auto-learn through addBars() instead.
bool folderHasHistory(const fs::path& folder) {
    std::error_code ec;
    if (fs::is_regular_file(folder, ec)) return folder.extension() == ".csv";
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().extension() == ".csv") return true;
    }
    return false;
}
// Step 5: below this a training run refuses with "too few bars" instead of
// training on almost nothing (trainBrainV2 itself needs ~5,800 bars to
// learn at all: first-learn share 0.3 x 480 bars, then 100 ATR samples).
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

// Rows the bot has already collected in bars_live.csv (comment and blank
// lines excluded), counted once at construction so totalBars() is right
// from the first snapshot even before addBars() sees a new bar.
std::size_t countLiveBars(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return 0;
    std::size_t rows = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        ++rows;
    }
    return rows;
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
    // v0.4.0: count the rows already collected, so totalBars() is right
    // from the first snapshot (addBars() only sees new bars).
    totalBars_ = countLiveBars(file_.parent_path() / kLiveBars);
    if (auto loaded = loadBrainV2File(file_)) {
        brain_ = std::move(*loaded);
        lastSavedBarMs_ = brain_.trainedUntilMs;
        if (log_) log_("[brain v2] loaded " + file_.string() + ", learned from " + std::to_string(brain_.trainedBars) + " bars");
    } else {
        if (log_) log_("[brain v2] no brain at " + file_.string() + " yet: learning from the bars in its folder");
        if (folderHasHistory(file_.parent_path())) startTraining(std::nullopt);
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
    // Step 6: a brain that records the erMode it trained under wins over
    // the panel's erMode (like the trained shift): the brain was validated
    // under that mode, so running it under another mode is unvalidated.
    // A brain without a recorded mode (erMode < 0) leaves the panel's
    // value in place. The stop loss is NOT overridden here -- the panel
    // governs the live risk control; the brain only records what it
    // trained under.
    muuk::core::BrainV2Config effective = config_;
    if (brain_.trained && brain_.erMode >= 0) effective.erMode = brain_.erMode;
    out.decision = muuk::core::decideWithBrainV2(brain_, out.situation, effective, account);
    return out;
}

void BrainKeeperV2::setConfig(muuk::core::BrainV2Config config) {
    std::scoped_lock lock(mutex_);
    config_ = std::move(config);
}

bool BrainKeeperV2::trained() const {
    std::scoped_lock lock(mutex_);
    return brain_.trained;
}

int64_t BrainKeeperV2::trainedBars() const {
    std::scoped_lock lock(mutex_);
    return brain_.trainedBars;
}

int64_t BrainKeeperV2::trainedUntilMs() const {
    std::scoped_lock lock(mutex_);
    return brain_.trainedUntilMs;
}

std::vector<muuk::core::BrainV2Cell> BrainKeeperV2::cells() const {
    std::scoped_lock lock(mutex_);
    return brain_.cells;
}

std::size_t BrainKeeperV2::totalBars() const {
    std::scoped_lock lock(mutex_);
    return totalBars_.load();
}

int BrainKeeperV2::newBars() const {
    std::scoped_lock lock(mutex_);
    return newBars_;
}

std::vector<bool> BrainKeeperV2::vetoedCells() const {
    std::scoped_lock lock(mutex_);
    std::vector<bool> out(81, false);
    if (!brain_.trained || brain_.cells.size() != 81) return out;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const muuk::core::BrainV2Cell& cell = brain_.cells[i];
        out[i] = cell.best.gapChoice >= 0 && (cell.score <= 0.0 || cell.lockRate > config_.maxLockRate);
    }
    return out;
}

std::optional<std::pair<int, double>> BrainKeeperV2::trainedShift() const {
    std::scoped_lock lock(mutex_);
    if (!brain_.trained || brain_.shiftAfterFills <= 0 || brain_.stepAtr <= 0.0) return std::nullopt;
    return std::pair<int, double>{brain_.shiftAfterFills, brain_.stepAtr};
}

std::optional<int> BrainKeeperV2::trainedErMode() const {
    std::scoped_lock lock(mutex_);
    if (!brain_.trained || brain_.erMode < 0) return std::nullopt;
    return brain_.erMode;
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
    totalBars_.fetch_add(static_cast<std::size_t>(added));  // v0.4.0: keep the total row count
    if (newBars_ >= kNewBarsToLearn) {
        newBars_ = 0;
        startTraining(std::nullopt);
    }
}

void BrainKeeperV2::trainNow(std::optional<fs::path> history) {
    try {
        startTraining(std::move(history));
    } catch (const std::exception& ex) {
        if (log_) log_(std::string("[brain v2] train now failed to start: ") + ex.what());
    }
}

BrainTrainingProgress BrainKeeperV2::trainingProgress() const {
    std::scoped_lock lock(mutex_);
    return progress_;
}

BrainTrainingResult BrainKeeperV2::lastTraining() const {
    std::scoped_lock lock(mutex_);
    return lastTraining_;
}

void BrainKeeperV2::startTraining(std::optional<fs::path> history) {
    if (learning_.exchange(true)) return;  // a run is already going
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, history = std::move(history)] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // never ahead of quotes and fills
#endif
        // Finish a run: record its outcome (visible to the panel), clear
        // the progress, log one line, and only then mark it not learning --
        // the panel polls learning() first, so the result must already be
        // in place when that flips.
        auto finish = [&](BrainTrainingResult result, const std::string& logLine) {
            {
                std::scoped_lock lock(mutex_);
                lastTraining_ = result;
                lastTry_ = result.summary;
                progress_ = BrainTrainingProgress{};
            }
            if (log_ && !logLine.empty()) log_(logLine);
            learning_ = false;
        };
        std::size_t barCount = 0;
        try {
            const fs::path source = history ? *history : file_.parent_path();
            const HistoryBarsV2 bars = loadHistoryFolderV2(source);
            barCount = bars.bars.size();
            {
                std::scoped_lock lock(mutex_);
                progress_ = BrainTrainingProgress{"loading", 0, 1, barCount};
            }
            BrainTrainingResult result;
            result.ran = true;
            result.bars = barCount;
            if (barCount < kMinTrainingBars) {
                // A fresh install has almost no bars yet: refuse honestly
                // instead of training on nothing and adopting a weak brain.
                result.summary = "too few bars to train on: " + std::to_string(barCount) + " (need at least " +
                                 std::to_string(kMinTrainingBars) + ")";
                finish(result, "[brain v2] " + result.summary);
                return;
            }
            muuk::core::BrainV2 current;
            muuk::core::BrainV2Config cfg;
            {
                // Both under the one lock: setConfig (from applySettings)
                // must never race the training thread's read of the config.
                std::scoped_lock lock(mutex_);
                current = brain_;
                cfg = config_;
            }
            muuk::core::BrainV2Training t = muuk::core::trainBrainV2(bars.bars, bars.spreads, current, cfg,
                [this](const char* stage, int done, int total) {
                    std::scoped_lock lock(mutex_);
                    progress_.stage = stage;
                    progress_.done = done;
                    progress_.total = total;
                });
            {
                std::ofstream report(file_.parent_path() / "training_report_v2.txt", std::ios::app);
                const std::time_t now = std::time(nullptr);
                char when[32];
                std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
                report << "=== " << when << "\n" << t.report << "\n";
                report << "trainer: " << (t.gatesPassed ? "gates PASSED" : "gates NOT PASSED")
                       << " -- the in-app trainer adopts only on the strict gates, whatever the walk-forward line above says\n";
            }
            result.gatesPassed = t.gatesPassed;
            if (t.brain.trained && t.gatesPassed) {
                // Step 5: adopt ONLY on the strict gates. The old rule
                // (t.better, walk-forward only) could adopt a brain that
                // fails the drawdown / locks / held-out gates.
                try {
                    saveBrainV2File(file_, t.brain);
                    {
                        std::scoped_lock lock(mutex_);
                        brain_ = t.brain;
                    }
                    result.adopted = true;
                } catch (const std::exception& ex) {
                    if (log_) log_(std::string("[brain v2] can't save: ") + ex.what());
                }
            }
            if (!t.tested.trained) {
                // trainBrainV2 itself refused to learn: its report's first
                // line says why ("too few bars to learn from: N"). Note
                // this is tested, not brain: the full brain is only built
                // when the run is better, so an honest gates failure
                // (trained, but not better) must report its numbers below,
                // not this line.
                std::string line = t.report;
                if (const std::size_t nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
                result.summary = line + " (a run needs ~5,800 bars to learn)";
            } else {
                char summary[360];
                std::snprintf(summary, sizeof(summary),
                              "%s: v2 %+.0f on %zu bars, %d locks, DD %.0f, %d/%d periods up, held-out %+.0f (%d locks)",
                              result.adopted ? "adopted" : "not adopted", t.brainTest.totalPnl, barCount,
                              t.brainTest.locks, t.brainTest.maxDrawdown, t.brainPeriodsUp, t.periods,
                              t.heldOutV2.totalPnl, t.heldOutV2.locks);
                result.summary = summary;
                result.summary += result.gatesPassed ? ", gates PASSED" : ", " + muuk::core::gatesFailureSummary(t);
                // v0.4.0: the same numbers, structured, so the Brain tab can
                // name the failed gates without parsing `summary`.
                result.better = t.better;
                result.ddGate = t.ddGate;
                result.lockGate = t.lockGate;
                result.periods = t.periods;
                result.brainPeriodsUp = t.brainPeriodsUp;
                result.totalPnl = t.brainTest.totalPnl;
                result.locks = t.brainTest.locks;
                result.maxDrawdown = t.brainTest.maxDrawdown;
                result.heldOutPnl = t.heldOutV2.totalPnl;
                result.heldOutLocks = t.heldOutV2.locks;
            }
            finish(result, "[brain v2] trained on " + std::to_string(barCount) + " bars: " + result.summary);
        } catch (const std::exception& ex) {
            // A training run must never take the live loop (or the app)
            // down with it: report the failure like any other outcome.
            BrainTrainingResult failed;
            failed.ran = true;
            failed.bars = barCount;
            failed.summary = std::string("training failed: ") + ex.what();
            finish(failed, "[brain v2] " + failed.summary);
        } catch (...) {
            BrainTrainingResult failed;
            failed.ran = true;
            failed.bars = barCount;
            failed.summary = "training failed (unknown error)";
            finish(failed, "[brain v2] " + failed.summary);
        }
    });
}

}  // namespace muuk::ctrader_fix
