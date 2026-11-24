#pragma once

// BrainJournal: Phase 1 memory for Brain v2 (see docs/brain-v2.md).
//
// One JSON line per brain decision that mattered, appended to
// muuk_history/<config name>.brain_journal.jsonl next to the account
// config (brainJournalFileFor in live_state.hpp). A line records the
// market situation the brain saw (cell buckets + ATR), the policy it
// ran on, and how the cycle ended -- or why the brain vetoed the cycle.
// Later phases read this file back to learn from live trading; Phase 1
// only writes it, and changes nothing about how the bot trades.
//
// Best-effort by design: appending never throws and never blocks the
// live loop for long (one append; an occasional rotation when the file
// passes its cap). A journal failure is silently skipped -- a basket
// must never fail to finalize because its journal line could not be
// written. Lines hold no credentials and no account numbers: only
// market numbers, the policy, and the outcome.

#include <cstddef>
#include <filesystem>
#include <string>

#include "muuk/core/market_brain_v2.hpp"

namespace muuk::ctrader_fix {

class BrainJournal {
public:
    static constexpr std::size_t kDefaultMaxBytes = 5 * 1024 * 1024;  // rotate past ~5 MB
    static constexpr std::size_t kDefaultKeepBytes = kDefaultMaxBytes / 2;  // keep the newest ~2.5 MB

    BrainJournal() = default;
    BrainJournal(std::size_t maxBytes, std::size_t keepBytes) : maxBytes_(maxBytes), keepBytes_(keepBytes) {}

    // Empty path = journaling off (the default until the controller sets a file).
    void setFile(std::filesystem::path file) { file_ = std::move(file); }

    // The cell a situation falls in (0..80): volatility x trend x chop x
    // session. Same layout as cellIndex() in engine/core/src/market_brain_v2.cpp.
    [[nodiscard]] static int cellIndex(const muuk::core::Situation& s) {
        return s.volatility * 27 + s.trend * 9 + s.chop * 3 + s.session;
    }

    // A brain-driven basket just closed. netReplay is the basket net in
    // replay units (the same units observeBasket learns in); legs is how
    // many positions the basket ran; closeReason is how it ended
    // ("profit target", "locked", ...). Never throws.
    void appendCycle(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Decision& decision,
                     double netReplay, bool locked, int legs, const std::string& closeReason);
    // The brain vetoed a cycle (decision.trade is false). One line per
    // veto episode -- the runner only calls this when it first logs the
    // veto, not on every retried tick. Never throws.
    void appendSkip(const muuk::core::SituationV2& situation, const muuk::core::BrainV2Decision& decision);

    // The JSON line itself (no trailing newline), for tests and for
    // callers that write the file another way. Never throws.
    [[nodiscard]] static std::string formatCycle(const muuk::core::SituationV2& situation,
                                                 const muuk::core::BrainV2Decision& decision, double netReplay,
                                                 bool locked, int legs, const std::string& closeReason);
    [[nodiscard]] static std::string formatSkip(const muuk::core::SituationV2& situation,
                                                const muuk::core::BrainV2Decision& decision);

private:
    void appendLine(const std::string& line);
    // Drops the oldest lines until the file fits keepBytes_, via a temp
    // file + rename. Only called when the file is past maxBytes_.
    void rotateLocked() noexcept;

    std::filesystem::path file_;
    std::size_t maxBytes_ = kDefaultMaxBytes;
    std::size_t keepBytes_ = kDefaultKeepBytes;
};

}  // namespace muuk::ctrader_fix
