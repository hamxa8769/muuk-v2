#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "muuk/ctrader_fix/brain_journal.hpp"

using namespace muuk::ctrader_fix;

namespace {

// Cell 59: high vol (2), no trend (0), medium chop (1), New York (2) --
// the biggest cell of the trained v0.2.5 brain (2,588 samples).
muuk::core::SituationV2 situation59() {
    muuk::core::SituationV2 s;
    s.base.volatility = 2;
    s.base.trend = 0;
    s.base.chop = 1;
    s.base.session = 2;
    s.base.atr = 1.15;
    s.base.known = true;
    s.known = true;
    return s;
}

muuk::core::BrainV2Decision tradeDecision() {
    muuk::core::BrainV2Decision d;
    d.trade = true;
    d.gap = 8.0;
    d.policy.gapChoice = 4;
    d.policy.lotMultiplier = 2.0;
    d.policy.maxLevels = 4;
    d.policy.exitFactor = 1.0;
    d.why = "cell 59: 2.0x/4/exit1.0, score 1.03";
    return d;
}

muuk::core::BrainV2Decision vetoDecision() {
    muuk::core::BrainV2Decision d;
    d.trade = false;
    d.gap = 0.0;
    d.why = "ATR 4.10 over 2.0x volHigh 1.15";
    return d;
}

std::vector<std::string> readLines(const std::filesystem::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

}  // namespace

TEST_CASE("Brain journal: a closed cycle formats to one JSON line with its cell and outcome") {
    const std::string line = BrainJournal::formatCycle(situation59(), tradeDecision(), 12.5, false, 3, "profit target");
    REQUIRE(!line.empty());
    const nlohmann::json j = nlohmann::json::parse(line);
    REQUIRE(j["event"].get<std::string>() == "cycle");
    REQUIRE(j["cell"].get<int>() == 59);
    REQUIRE(j["vol"].get<int>() == 2);
    REQUIRE(j["trend"].get<int>() == 0);
    REQUIRE(j["chop"].get<int>() == 1);
    REQUIRE(j["session"].get<int>() == 2);
    REQUIRE(j["atr"].get<double>() == Catch::Approx(1.15));
    REQUIRE(j["gap"].get<double>() == Catch::Approx(8.0));
    REQUIRE(j["gapChoice"].get<int>() == 4);
    REQUIRE(j["lotMultiplier"].get<double>() == Catch::Approx(2.0));
    REQUIRE(j["maxLevels"].get<int>() == 4);
    REQUIRE(j["exitFactor"].get<double>() == Catch::Approx(1.0));
    REQUIRE(j["firstSide"].get<int>() == 0);
    REQUIRE(j["net_replay"].get<double>() == Catch::Approx(12.5));
    REQUIRE(j["locked"].get<bool>() == false);
    REQUIRE(j["legs"].get<int>() == 3);
    REQUIRE(j["why"].get<std::string>() == "profit target");
    REQUIRE(j["ts"].is_string());
    REQUIRE(!j["ts"].get<std::string>().empty());
}

TEST_CASE("Brain journal: a vetoed cycle formats to one skip line with the why") {
    const std::string line = BrainJournal::formatSkip(situation59(), vetoDecision());
    REQUIRE(!line.empty());
    const nlohmann::json j = nlohmann::json::parse(line);
    REQUIRE(j["event"].get<std::string>() == "skip");
    REQUIRE(j["cell"].get<int>() == 59);
    REQUIRE(j["why"].get<std::string>() == "ATR 4.10 over 2.0x volHigh 1.15");
    REQUIRE(j["gap"].get<double>() == Catch::Approx(0.0));
    REQUIRE(j["gapChoice"].get<int>() == -1);
}

TEST_CASE("Brain journal: with no file set, appends are silently dropped") {
    BrainJournal journal;
    REQUIRE_NOTHROW(journal.appendCycle(situation59(), tradeDecision(), 12.5, false, 3, "profit target"));
    REQUIRE_NOTHROW(journal.appendSkip(situation59(), vetoDecision()));
}

TEST_CASE("Brain journal: lines append to a file and read back as JSON") {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "muuk_brain_journal_test.jsonl";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    BrainJournal journal;
    journal.setFile(file);
    journal.appendCycle(situation59(), tradeDecision(), 12.5, false, 3, "profit target");
    journal.appendSkip(situation59(), vetoDecision());

    const std::vector<std::string> lines = readLines(file);
    REQUIRE(lines.size() == 2);
    const nlohmann::json first = nlohmann::json::parse(lines[0]);
    REQUIRE(first["event"].get<std::string>() == "cycle");
    REQUIRE(first["cell"].get<int>() == 59);
    REQUIRE(first["net_replay"].get<double>() == Catch::Approx(12.5));
    const nlohmann::json second = nlohmann::json::parse(lines[1]);
    REQUIRE(second["event"].get<std::string>() == "skip");
    REQUIRE(second["cell"].get<int>() == 59);

    std::filesystem::remove(file, ec);
}

TEST_CASE("Brain journal: rotation keeps a big file under its cap") {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "muuk_brain_journal_rotation_test.jsonl";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    // Tiny limits so the test rotates without writing megabytes.
    BrainJournal journal(1200, 600);
    journal.setFile(file);
    for (int i = 0; i < 10; ++i) {
        journal.appendCycle(situation59(), tradeDecision(), 12.5, false, 3, "profit target");
    }

    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    REQUIRE(!ec);
    REQUIRE(size <= 1200 + 400);  // cap, plus at most one fresh line
    const std::vector<std::string> lines = readLines(file);
    REQUIRE(!lines.empty());
    for (const std::string& line : lines) {
        const nlohmann::json j = nlohmann::json::parse(line);
        REQUIRE(j["event"].get<std::string>() == "cycle");
    }

    std::filesystem::remove(file, ec);
}
