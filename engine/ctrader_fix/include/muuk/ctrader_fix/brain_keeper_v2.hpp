#pragma once

// BrainKeeperV2: the live side of Brain v2 (engine/core/market_brain_v2.hpp).
//
// What it does: holds the v2 brain the runner asks, keeps every finished
// bar the bot sees in bars_live.csv next to the brain file, and after each
// 1,440 new bars re-trains on a low-priority background thread. A new brain
// replaces the one in use only when the strict gates pass (better than fixed
// and v1 walking forward, drawdown and locks capped, >=5 periods up,
// held-out positive) -- never by force, never from one basket, never on a
// walk-forward-only "better". The dashboard Train button (Step 5) starts the
// same run on demand through trainNow(), on the bars the bot collected or on
// a history CSV the user points at. After each closed basket the runner
// calls observeBasket() so the brain's online learning can veto a cell that
// just lost badly; online learning can never invent a policy.
//
// Wired into LiveRunner (live_runner.cpp): the runner calls
// decideForCycle() before a new cycle opens and observeBasket() when one
// closes. The brain file is the plain-text format of serializeBrainV2()
// (brain_v2.txt), not the v1 brain.json format.

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "muuk/core/market_brain_v2.hpp"

namespace muuk::ctrader_fix {

// v0.4.0: in the header (not only the .cpp) so the dashboard can quote the
// same numbers ("need at least 2,000", "auto-retrain at 1,440") instead of
// hard-coding them.
inline constexpr int kNewBarsToLearn = 1440;  // a day of minutes
inline constexpr std::size_t kMinTrainingBars = 2000;

// One-minute bars with their spreads, from MT5's history export format:
// optional "# ...,point=..,gmtOffset=.." line, then time,open,high,low,close,ticks,spreadPoints.
struct HistoryBarsV2 {
    std::vector<muuk::core::Bar> bars;
    std::vector<double> spreads;  // in price
};
// Every .csv in the folder, merged by time; the newest copy of a bar wins.
[[nodiscard]] HistoryBarsV2 loadHistoryFolderV2(const std::filesystem::path& folder);

[[nodiscard]] std::optional<muuk::core::BrainV2> loadBrainV2File(const std::filesystem::path& file);
void saveBrainV2File(const std::filesystem::path& file, const muuk::core::BrainV2& brain);

// A v2 decision and the market situation it was taken in.
struct BrainV2Cycle {
    muuk::core::BrainV2Decision decision;
    muuk::core::SituationV2 situation;
};

// Live progress of a training run, for the Brain panel. stage is "" when
// idle; while running it is one of "loading", "walk-forward", "fixed",
// "held-out", "final" (the stage names trainBrainV2 reports).
struct BrainTrainingProgress {
    std::string stage;
    int done = 0;
    int total = 0;
    std::size_t bars = 0;  // bars the run is training on
};

// The outcome of the last finished training run (automatic or manual).
struct BrainTrainingResult {
    bool ran = false;          // a run finished (false until the first one)
    bool adopted = false;      // the new brain passed the gates and was saved + adopted
    bool gatesPassed = false;  // the strict gates (see trainBrainV2)
    std::size_t bars = 0;      // bars the run trained on
    std::string summary;       // one line with the numbers + gates verdict, for the panel
    // v0.4.0 (gap 3): the gate fields, so the panel names the failed gates
    // without parsing `summary`. Defaults when no gates ran (too few bars).
    bool better = false;
    bool ddGate = false;
    bool lockGate = false;
    int periods = 0;
    int brainPeriodsUp = 0;
    double totalPnl = 0.0;
    int locks = 0;
    double maxDrawdown = 0.0;
    double heldOutPnl = 0.0;
    int heldOutLocks = 0;
};

class BrainKeeperV2 {
public:
    using Log = std::function<void(const std::string&)>;

    BrainKeeperV2(std::filesystem::path brainFile, muuk::core::BrainV2Config config, Log log);
    ~BrainKeeperV2();

    // What v2 says about a new cycle. recentOutcome: -1 if the last closed
    // basket lost or locked, +1 if it won, 0 if there is none yet.
    [[nodiscard]] muuk::core::BrainV2Decision decide(const std::deque<muuk::core::Bar>& oneMinuteBars, int recentOutcome,
                                                     double spread, double medianSpread,
                                                     const muuk::core::BrainAccountState& account = {}) const;
    // The decision together with the situation it was taken in, so the
    // runner can hand the *same* situation back to observeBasket() when
    // the cycle closes (the bars will have moved on by then). decide()
    // above is this with the situation dropped.
    [[nodiscard]] BrainV2Cycle decideForCycle(const std::deque<muuk::core::Bar>& oneMinuteBars, int recentOutcome,
                                              double spread, double medianSpread,
                                              const muuk::core::BrainAccountState& account = {}) const;
    [[nodiscard]] bool trained() const;
    // Replace the config the next decision (and the next basket
    // observation) uses -- the risk knob and the ER mode, applied from the
    // settings without restarting. The loaded brain and its live vetoes
    // are untouched. Step 4: the runner calls this from applySettings.
    void setConfig(muuk::core::BrainV2Config config);
    // True while the background re-training thread is running: the runner
    // must not replace (and so join) this keeper until it stops.
    [[nodiscard]] bool learning() const { return learning_.load(); }
    // Train now, in the background, on the bars in `history` (a CSV file or
    // a folder of them) or -- when empty -- on the bars the bot collected in
    // the brain folder (bars_live.csv). The same run the automatic
    // re-training does, under the same config, with the same strict-gates
    // adoption rule. No-op while a run is already going; never throws.
    // A fresh install has almost no bars yet: the run then refuses with a
    // "too few bars" result instead of training on nothing.
    void trainNow(std::optional<std::filesystem::path> history = std::nullopt);
    // Live progress of the run in progress (stage "" when idle).
    [[nodiscard]] BrainTrainingProgress trainingProgress() const;
    // The outcome of the last finished run (automatic or manual).
    [[nodiscard]] BrainTrainingResult lastTraining() const;
    // What the loaded brain learned from -- so a re-read can tell a new
    // brain file from the same one written again.
    [[nodiscard]] int64_t trainedBars() const;
    [[nodiscard]] int64_t trainedUntilMs() const;
    // v0.4.0: copies for the Brain tab grid + Trainer card. All const, all
    // under mutex_; the dashboard never touches brain_ itself.
    // A copy of the loaded brain's 81 cells (empty when untrained).
    [[nodiscard]] std::vector<muuk::core::BrainV2Cell> cells() const;
    // Bars the bot has collected in total (rows in bars_live.csv).
    [[nodiscard]] std::size_t totalBars() const;
    // Bars collected since the last training run started (0 after a run starts).
    [[nodiscard]] int newBars() const;
    // The live veto state per cell: true where online learning turned the
    // cell to no-trade. (Computed from the brain copy: score <= 0 or
    // lockRate > maxLockRate on a cell whose best.gapChoice >= 0.)
    [[nodiscard]] std::vector<bool> vetoedCells() const;  // size 81, false when untrained
    // The shift the loaded brain was trained with (Step 2, decision D12):
    // {fills per step, step in ATR multiples}. Nothing when the brain is
    // not trained or its file predates Step 2 (no shift fields) -- the
    // runner then keeps the config shift.
    [[nodiscard]] std::optional<std::pair<int, double>> trainedShift() const;

    // Step 6: the ER gate mode the loaded brain trained under, when it
    // recorded one (see decideForCycle). Nothing for an old brain file.
    [[nodiscard]] std::optional<int> trainedErMode() const;
    // One line for the operator: what it learned from, how its last try went.
    [[nodiscard]] std::string status() const;
    // A basket just closed: let the online learning see it (veto only).
    void observeBasket(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Policy& used, double pnl, bool locked);
    // Keeps the finished bars it hasn't seen; learns again when it's time. spread: in price.
    void addBars(const std::deque<muuk::core::Bar>& oneMinuteBars, double spread);

private:
    // The one thread body for the automatic re-train (after 1,440 new bars)
    // and the manual trainNow(): loads the bars, guards the minimum,
    // trains with progress, and adopts only on the strict gates.
    void startTraining(std::optional<std::filesystem::path> history);

    std::filesystem::path file_;
    muuk::core::BrainV2Config config_;
    Log log_;
    mutable std::mutex mutex_;
    muuk::core::BrainV2 brain_;
    int64_t lastSavedBarMs_ = 0;
    int newBars_ = 0;
    std::atomic<std::size_t> totalBars_{0};  // v0.4.0: rows in bars_live.csv (counted at construction, then added to)
    std::string lastTry_;
    BrainTrainingProgress progress_;
    BrainTrainingResult lastTraining_;
    std::atomic<bool> learning_{false};
    std::thread thread_;
};

}  // namespace muuk::ctrader_fix
