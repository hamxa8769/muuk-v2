#pragma once

#include <deque>
#include <optional>
#include <string>

#include "muuk/core/market_read.hpp"

namespace muuk::core {

// The regime-range strategy (off by default; docs/regime-range.md): a new cycle
// only in a ranging market, Buy Stop and Sell Stop outside a range worked out
// from the data, the range worked out again after 4 trades, a lock at the 9th.
struct RegimeConfig {
    bool enabled = false;
    int barMinutes = 5;     // the bars it reads, made from the one-minute ones
    int lookbackBars = 24;  // of those bars, for the tick volume
    int rangeMinutes = 20;  // the range starts from the high and low of these last one-minute bars

    // Trending when more than maxTrendVotes of these say so.
    int maxTrendVotes = 2;
    double adxTrendMin = 25.0;      // ADX(14) at or over this
    double chopRangeMin = 50.0;     // Choppiness(14) under this
    double emaGapAtr = 1.0;         // |EMA8 - EMA21| at or over this x ATR
    double smaSlopeAtr = 1.0;       // SMA(20) moved this x ATR in 5 bars
    double rsiLow = 30.0;           // RSI(14) under this, or over rsiHigh
    double rsiHigh = 70.0;
    double volumeSpike = 2.0;       // last bar's ticks at or over this x their average

    double bufferAtr = 0.5;         // the orders' distance beyond the range, x ATR
    double minGap = 2.0;            // the distance between Buy Stop and Sell Stop, always within these
    double maxGap = 15.0;
    double minDistance = 0.50;      // the least distance from the price to either order

    int maxPositions = 8;           // the next one is never opened: the lock instead
    // No new cycle if a full lock would lose more than this % of the balance, in
    // the account's own money (cent or standard). 0 = off.
    double maxLockRiskPercent = 25.0;
    // Steps: this many trades share a step's two lines; then both orders move
    // out to a wider step, gap + stepAtr x the one-minute ATR (at least $1, at
    // most maxGap), around the same middle.
    int shiftAfterFills = 3;
    double stepAtr = 2.0;
    double moveMinPoints = 0.25;    // evaluated on every tick; orders move only for at least this...
    int moveMinSeconds = 2;         // ...and at most once in this long (brokers limit order changes)

    // The CME order book and option chain arrive here as JSON lines. With
    // requireFeed no new cycle starts and no order moves while they're stale.
    bool requireFeed = true;
    // A brain (market_brain.hpp) learned from history chooses the gap and
    // whether to trade, and learns again by itself. Empty = no brain.
    std::string brainFile;
    // No brain has passed its test yet: true = no new cycle; false = the
    // worked-out gap, learning on in the background (for demo testing).
    bool brainRequired = true;
    int feedPort = 5310;
    int bookMaxAgeSeconds = 5;
    int optionsMaxAgeSeconds = 900;
};

enum class MarketRegime { Unknown, Ranging, Trending };

// What the bars say, recalculated once per finished bar.
struct RegimeRead {
    MarketRegime regime = MarketRegime::Unknown;
    int trendVotes = 0;
    double atr = 0.0;
    double atrMinute = 0.0;     // of the one-minute bars: the scale of the gap
    double adx = 0.0;
    double rsi = 50.0;
    double choppiness = 50.0;
    double emaFast = 0.0;
    double emaSlow = 0.0;
    double sma = 0.0;
    double bollingerUpper = 0.0;  // of the one-minute bars
    double bollingerLower = 0.0;
    double volumeRatio = 1.0;   // last bar's ticks / their average (1 when unknown)
    double recentHigh = 0.0;    // over rangeMinutes
    double recentLow = 0.0;
    std::string why;            // one line, for the operator
};

// Where the big resting orders and the open interest are, in the broker's
// prices (0 = none known). Only ever makes the range wider.
struct ExternalLevels {
    bool bookFresh = false;
    bool optionsFresh = false;
    double bidWall = 0.0;
    double askWall = 0.0;
    double putWall = 0.0;
    double callWall = 0.0;
};

struct RegimeRange {
    double low = 0.0;       // the range itself
    double high = 0.0;
    double sellStop = 0.0;  // the orders, outside it
    double buyStop = 0.0;
    bool clamped = false;   // minGap / maxGap decided the distance
};

// The regime placement for one cycle: the read behind it plus where the
// orders go. The replay and the live runner share this, so a cycle places
// the same range in training and live.
struct RegimePlacement {
    RegimeRead read;
    RegimeRange range;
};

// Place one cycle's two orders from the bars ending at the cycle, in a
// ranging market only: the orders sit just outside the market's own recent
// high/low (its Bollinger band when calm), at least `gap` apart -- the
// brain's gap is a floor, not the distance. Nothing in a trend (the range
// is stale there) or with too few bars to read a regime: the caller falls
// back to a centered range.
[[nodiscard]] std::optional<RegimePlacement> placeRegimeRange(const std::deque<Bar>& bars, double mid, double gap);

// One-minute bars -> bars of `minutes`, oldest first.
[[nodiscard]] std::deque<Bar> aggregateBars(const std::deque<Bar>& bars, int minutes);

// Each 0 (RSI 50, Choppiness 50) with too few bars; bars oldest first.
[[nodiscard]] double atrOf(const std::deque<Bar>& bars, int period);
[[nodiscard]] double adxOf(const std::deque<Bar>& bars, int period);
[[nodiscard]] double rsiOf(const std::deque<Bar>& bars, int period);
[[nodiscard]] double choppinessOf(const std::deque<Bar>& bars, int period);
[[nodiscard]] double smaOf(const std::deque<Bar>& bars, int period, int barsAgo = 0);
[[nodiscard]] double emaOf(const std::deque<Bar>& bars, int period);

// One-minute bars in. Unknown until there are enough of them.
[[nodiscard]] RegimeRead readRegime(const std::deque<Bar>& oneMinuteBars, const RegimeConfig& config);

// The range and its orders now. fillsLow / fillsHigh: the lowest and highest
// entry of the basket's trades, which the range has to contain (0 = none).
// The price is always between the orders; maxGap wins over the fills.
// chosenGap > 0 (the brain's, before any fill) replaces the worked-out distance.
[[nodiscard]] RegimeRange computeRegimeRange(const RegimeRead& read, double mid, const ExternalLevels& levels,
                                             double fillsLow, double fillsHigh, const RegimeConfig& config,
                                             double chosenGap = 0.0);

}  // namespace muuk::core
