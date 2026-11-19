#pragma once

// BrainKeeperV2: the live side of Brain v2 (engine/core/market_brain_v2.hpp).
//
// What it does: holds the v2 brain the runner asks, keeps every finished
// bar the bot sees in bars_live.csv next to the brain file, and after each
// 1,440 new bars re-trains on a low-priority background thread. A new brain
// replaces the one in use only if trainBrainV2 says it is better walking
// forward — never by force, never from one basket. After each closed basket
// the runner calls observeBasket() so the brain's online learning can veto
// a cell that just lost badly; online learning can never invent a policy.
//
// NOT yet wired into LiveRunner: the regime mode (LiveConfig::regime) is
// not in this tree, so decide() is the wiring point for that step — the
// runner will call decide() for a new cycle and observeBasket() when one
// closes. The brain file is the plain-text format of serializeBrainV2()
// (brain_v2.txt), not the v1 brain.json format.

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "muuk/core/market_brain_v2.hpp"

namespace muuk::ctrader_fix {

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
    [[nodiscard]] bool trained() const;
    // One line for the operator: what it learned from, how its last try went.
    [[nodiscard]] std::string status() const;
    // A basket just closed: let the online learning see it (veto only).
    void observeBasket(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Policy& used, double pnl, bool locked);
    // Keeps the finished bars it hasn't seen; learns again when it's time. spread: in price.
    void addBars(const std::deque<muuk::core::Bar>& oneMinuteBars, double spread);

private:
    void learnInBackground();

    std::filesystem::path file_;
    muuk::core::BrainV2Config config_;
    Log log_;
    mutable std::mutex mutex_;
    muuk::core::BrainV2 brain_;
    int64_t lastSavedBarMs_ = 0;
    int newBars_ = 0;
    std::string lastTry_;
    std::atomic<bool> learning_{false};
    std::thread thread_;
};

}  // namespace muuk::ctrader_fix
