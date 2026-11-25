#include "muuk/ctrader_fix/brain_journal.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace muuk::ctrader_fix {

namespace {

// UTC timestamp with milliseconds, e.g. 2026-11-24T18:05:33.123Z.
std::string isoNow() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

// The fields every line carries: when, which cell, what the market
// looked like, and the policy the brain ran on (or would have run on).
nlohmann::json baseLine(const std::string& event, const muuk::core::SituationV2& situation,
                        const muuk::core::BrainV2Decision& decision) {
    nlohmann::json j;
    j["ts"] = isoNow();
    j["event"] = event;
    j["cell"] = BrainJournal::cellIndex(situation.base);
    j["vol"] = situation.base.volatility;
    j["trend"] = situation.base.trend;
    j["chop"] = situation.base.chop;
    j["session"] = situation.base.session;
    j["atr"] = situation.base.atr;
    j["gap"] = decision.gap;
    j["gapChoice"] = decision.policy.gapChoice;
    j["lotMultiplier"] = decision.policy.lotMultiplier;
    j["maxLevels"] = decision.policy.maxLevels;
    j["exitFactor"] = decision.policy.exitFactor;
    j["firstSide"] = decision.firstSide;  // Step 3: -1 Sell first, 0 both stops, +1 Buy first
    return j;
}

}  // namespace

std::string BrainJournal::formatCycle(const muuk::core::SituationV2& situation,
                                      const muuk::core::BrainV2Decision& decision, double netReplay, bool locked,
                                      int legs, const std::string& closeReason) {
    try {
        nlohmann::json j = baseLine("cycle", situation, decision);
        j["net_replay"] = netReplay;
        j["locked"] = locked;
        j["legs"] = legs;
        j["why"] = closeReason;
        return j.dump();
    } catch (...) {
        return {};
    }
}

std::string BrainJournal::formatSkip(const muuk::core::SituationV2& situation,
                                     const muuk::core::BrainV2Decision& decision) {
    try {
        nlohmann::json j = baseLine("skip", situation, decision);
        j["why"] = decision.why;
        return j.dump();
    } catch (...) {
        return {};
    }
}

void BrainJournal::appendCycle(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Decision& decision,
                               double netReplay, bool locked, int legs, const std::string& closeReason) {
    const std::string line = formatCycle(situation, decision, netReplay, locked, legs, closeReason);
    if (!line.empty()) appendLine(line);
}

void BrainJournal::appendSkip(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Decision& decision) {
    const std::string line = formatSkip(situation, decision);
    if (!line.empty()) appendLine(line);
}

void BrainJournal::appendLine(const std::string& line) {
    if (file_.empty()) return;
    try {
        std::error_code ec;
        if (std::filesystem::exists(file_, ec) && std::filesystem::file_size(file_, ec) > maxBytes_) {
            rotateLocked();
        }
        if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
        std::ofstream out(file_, std::ios::app | std::ios::binary);
        if (!out) return;
        out << line << '\n';
    } catch (...) {
        // Best-effort only: a journal failure must never break the live loop.
    }
}

void BrainJournal::rotateLocked() noexcept {
    try {
        std::ifstream in(file_, std::ios::binary);
        if (!in) return;
        std::vector<std::string> lines;
        std::string l;
        std::size_t bytes = 0;
        while (std::getline(in, l)) {
            lines.push_back(l);
            bytes += l.size() + 1;
        }
        in.close();
        // Drop the oldest lines until the kept tail fits keepBytes_.
        std::size_t drop = 0;
        while (drop < lines.size() && bytes > keepBytes_) {
            bytes -= lines[drop].size() + 1;
            ++drop;
        }
        if (drop == 0) return;
        const std::filesystem::path tmp = file_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            for (std::size_t i = drop; i < lines.size(); ++i) out << lines[i] << '\n';
            out.flush();
            if (!out) return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file_, ec);
    } catch (...) {
        // Rotation is best-effort; the next append still goes through.
    }
}

}  // namespace muuk::ctrader_fix
