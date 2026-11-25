// The in-app trainer (Step 5): BrainKeeperV2::trainNow() -- a Train button
// run on the bars the bot collected or on a history CSV, in the background,
// adopting a new brain only when the strict gates pass.
//
// The fixtures are generated procedurally in this file, so the tests need
// no history file:
//   - sharpStormBars(): calm 18k + a sharp storm 3k + calm 9k. The storm is
//     a mean-reverting walk with a $7 step around a slowly falling centre:
//     the ATR window lags, the gap stays stale-narrow, and the no-stop v1
//     replay locks once for -$2,535 while the stopped v2 walks forward at
//     +$1,153 with no locks and a +$314 held-out. Gates PASSED (verified
//     against the CLI trainer). Deterministic: std::mt19937_64 (its output
//     sequence is standardised) with our own uniform, so the bars are
//     identical on libstdc++ and MSVC. The bars start at the same epoch
//     the CLI verification used: the session buckets come from the UTC
//     hour, so the absolute time matters.
//   - rangingBars(12000): the ranging recipe from test_market_brain_v2:
//     v2 walks forward at +$209 vs fixed +$569 and v1 +$525 -- gates NOT
//     PASSED, so nothing is adopted.
//   - rangingBars(500): below the 2000-bar guard -- the run refuses.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/brain_keeper_v2.hpp"

using namespace muuk::ctrader_fix;
using namespace muuk::core;

namespace {

// The epoch the CLI verification used (2023-11-14 22:13:20 UTC): the
// session buckets come from the UTC hour, so the fixtures must start here.
constexpr int64_t kFixtureT0Ms = 1700000000LL * 1000;

Bar fixtureBar(int64_t index, double prev, double close) {
    return Bar{kFixtureT0Ms + index * 60'000LL, prev, std::max(prev, close) + 0.2,
               std::min(prev, close) - 0.2, close, 20.0};
}

std::vector<Bar> rangingBars(int count, double swing = 6.0, double noise = 0.2) {
    std::vector<Bar> out;
    double prev = 4000.0;
    for (int i = 0; i < count; ++i) {
        const double c = 4000.0 + swing * std::sin(i * 0.05) + noise * std::sin(i * 1.3);
        out.push_back(fixtureBar(i, prev, c));
        prev = c;
    }
    return out;
}

double uniform01(std::mt19937_64& rng) {
    return static_cast<double>(rng() - rng.min()) / static_cast<double>(rng.max() - rng.min());
}

std::vector<Bar> sharpStormBars() {
    std::vector<double> closes;
    closes.reserve(29999);
    for (int i = 0; i < 18000; ++i)
        closes.push_back(4000.0 + 6.0 * std::sin(i * 0.05) + 0.2 * std::sin(i * 1.3));
    {
        std::mt19937_64 rng(2);
        double p = closes.back();
        double center = p;
        for (int i = 0; i < 3000; ++i) {
            center -= 0.03;
            p += -0.10 * (p - center) + (-7.0 + 14.0 * uniform01(rng));
            closes.push_back(p);
        }
    }
    {
        const double base = closes.back();
        for (int i = 1; i < 9000; ++i)
            closes.push_back(base + 6.0 * std::sin(i * 0.05) + 0.2 * std::sin(i * 1.3));
    }
    std::vector<Bar> out;
    out.reserve(closes.size());
    double prev = closes[0];
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double c = closes[i];
        out.push_back(fixtureBar(static_cast<int64_t>(i), prev, c));
        prev = c;
    }
    return out;
}

void writeBarsCsv(const std::filesystem::path& file, const std::vector<Bar>& bars) {
    std::ofstream out(file);
    out << "# point=0.01,gmtOffset=0\n";
    for (const Bar& b : bars) {
        char line[200];
        std::snprintf(line, sizeof(line), "%lld,%.5f,%.5f,%.5f,%.5f,%.0f,0\n",
                      static_cast<long long>(b.startMs / 1000), b.open, b.high, b.low, b.close, b.tickVolume);
        out << line;
    }
}

// The config the keeper tests train under: erMode 0, the Balanced caps,
// and an EXPLICIT $50 basket stop loss in replay units. The stop is set on
// purpose here -- the fixtures were tuned under it (the storm seed was
// picked from 7 passing seeds under a stop), and these tests exercise the
// adopt/refuse mechanics, not the live default. The live default is OFF
// since Step 6b (the trainer uses the panel's stop via brainV2ConfigFor),
// and the stop path itself stays tested by this config. The shift stays
// at the Step 2 winner (3 fills x 2.0 ATR) from the brain config.
BrainV2Config trainerConfig() {
    BrainV2Config cfg;
    cfg.erMode = 0;
    cfg.stopLoss = 50.0;
    return cfg;
}

bool waitUntilNotLearning(const BrainKeeperV2& keeper, int timeoutSeconds = 180) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (keeper.learning()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

struct TempDir {
    std::filesystem::path dir;
    explicit TempDir(const char* name) : dir(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("in-app trainer: a run on too few bars refuses instead of training on nothing") {
    TempDir tmp("muuk-trainer-too-few-test");
    // The keeper is built on the empty folder first: no auto-learn starts
    // (nothing to learn from), so this run is the manual one.
    BrainKeeperV2 keeper(tmp.dir / "brain_v2.txt", trainerConfig(), nullptr);
    writeBarsCsv(tmp.dir / "bars.csv", rangingBars(500));
    keeper.trainNow(tmp.dir / "bars.csv");
    REQUIRE(waitUntilNotLearning(keeper));
    const BrainTrainingResult last = keeper.lastTraining();
    REQUIRE(last.ran);
    REQUIRE_FALSE(last.adopted);
    REQUIRE_FALSE(last.gatesPassed);
    REQUIRE(last.bars == 500);
    REQUIRE(last.summary.find("too few bars") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(tmp.dir / "brain_v2.txt"));
    REQUIRE_FALSE(keeper.trained());
}

TEST_CASE("in-app trainer: a run that fails the gates adopts nothing") {
    TempDir tmp("muuk-trainer-gates-fail-test");
    BrainKeeperV2 keeper(tmp.dir / "brain_v2.txt", trainerConfig(), nullptr);
    writeBarsCsv(tmp.dir / "bars.csv", rangingBars(12000));
    keeper.trainNow(tmp.dir / "bars.csv");
    REQUIRE(waitUntilNotLearning(keeper));
    const BrainTrainingResult last = keeper.lastTraining();
    REQUIRE(last.ran);
    REQUIRE_FALSE(last.gatesPassed);
    REQUIRE_FALSE(last.adopted);
    REQUIRE(last.summary.find("NOT PASSED") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(tmp.dir / "brain_v2.txt"));
    REQUIRE_FALSE(keeper.trained());
}

TEST_CASE("in-app trainer: a run that passes the gates adopts the new brain, and the live loop keeps deciding meanwhile") {
    TempDir tmp("muuk-trainer-gates-pass-test");
    BrainKeeperV2 keeper(tmp.dir / "brain_v2.txt", trainerConfig(), nullptr);
    const std::vector<Bar> bars = sharpStormBars();
    writeBarsCsv(tmp.dir / "bars.csv", bars);
    keeper.trainNow(tmp.dir / "bars.csv");
    // While the run goes, the live loop keeps deciding on the current
    // (untrained) brain: decideForCycle must return promptly, never block
    // behind the training thread.
    std::deque<Bar> recent(bars.begin(), bars.begin() + 240);
    bool sawProgress = false;
    int decisions = 0;
    const auto started = std::chrono::steady_clock::now();
    while (keeper.learning()) {
        if (!keeper.trainingProgress().stage.empty()) sawProgress = true;
        const auto before = std::chrono::steady_clock::now();
        const BrainV2Cycle cycle = keeper.decideForCycle(recent, 0, 0.01, 0.01);
        REQUIRE(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
        (void)cycle;
        ++decisions;
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(180)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(waitUntilNotLearning(keeper));
    REQUIRE(decisions > 0);
    REQUIRE(sawProgress);
    const BrainTrainingResult last = keeper.lastTraining();
    REQUIRE(last.ran);
    REQUIRE(last.gatesPassed);
    REQUIRE(last.adopted);
    REQUIRE(last.bars == bars.size());
    REQUIRE(last.summary.find("PASSED") != std::string::npos);
    REQUIRE(std::filesystem::exists(tmp.dir / "brain_v2.txt"));
    REQUIRE(keeper.trained());
    const std::optional<BrainV2> loaded = loadBrainV2File(tmp.dir / "brain_v2.txt");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->trained);
    REQUIRE(loaded->trainedBars > 0);
}
