#include "muuk/core/regime_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace muuk::core {

namespace {

constexpr int kPeriod = 14;
constexpr int kBollingerPeriod = 20;
constexpr int kMinBars = 2 * kPeriod + 2;  // ADX needs two periods

double trueRange(const Bar& bar, const Bar& previous) {
    return std::max({bar.high - bar.low, std::fabs(bar.high - previous.close), std::fabs(bar.low - previous.close)});
}

// Wilder's smoothing: the first value is an average, then (old x (n-1) + new) / n.
double wilder(const std::vector<double>& values, int period) {
    if (period <= 0 || static_cast<int>(values.size()) < period) return 0.0;
    double avg = 0.0;
    for (int i = 0; i < period; ++i) avg += values[i];
    avg /= period;
    for (std::size_t i = period; i < values.size(); ++i) avg = (avg * (period - 1) + values[i]) / period;
    return avg;
}

}  // namespace

std::deque<Bar> aggregateBars(const std::deque<Bar>& bars, int minutes) {
    std::deque<Bar> out;
    const int64_t size = std::max(1, minutes) * 60'000LL;
    for (const Bar& b : bars) {
        const int64_t start = b.startMs - (((b.startMs % size) + size) % size);
        if (out.empty() || out.back().startMs != start) {
            out.push_back(Bar{start, b.open, b.high, b.low, b.close, b.tickVolume});
            continue;
        }
        Bar& into = out.back();
        into.high = std::max(into.high, b.high);
        into.low = std::min(into.low, b.low);
        into.close = b.close;
        into.tickVolume += b.tickVolume;
    }
    return out;
}

double atrOf(const std::deque<Bar>& bars, int period) {
    std::vector<double> ranges;
    for (std::size_t i = 1; i < bars.size(); ++i) ranges.push_back(trueRange(bars[i], bars[i - 1]));
    return wilder(ranges, period);
}

double adxOf(const std::deque<Bar>& bars, int period) {
    if (period <= 0 || static_cast<int>(bars.size()) < 2 * period + 1) return 0.0;
    double smoothTr = 0.0;
    double smoothPlus = 0.0;
    double smoothMinus = 0.0;
    std::vector<double> dx;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const double up = bars[i].high - bars[i - 1].high;
        const double down = bars[i - 1].low - bars[i].low;
        const double plus = up > down && up > 0.0 ? up : 0.0;
        const double minus = down > up && down > 0.0 ? down : 0.0;
        const double tr = trueRange(bars[i], bars[i - 1]);
        if (static_cast<int>(i) <= period) {
            smoothTr += tr;
            smoothPlus += plus;
            smoothMinus += minus;
            if (static_cast<int>(i) < period) continue;
        } else {
            smoothTr += tr - smoothTr / period;
            smoothPlus += plus - smoothPlus / period;
            smoothMinus += minus - smoothMinus / period;
        }
        const double sum = smoothPlus + smoothMinus;
        dx.push_back(sum > 0.0 ? 100.0 * std::fabs(smoothPlus - smoothMinus) / sum : 0.0);
    }
    return wilder(dx, period);
}

double rsiOf(const std::deque<Bar>& bars, int period) {
    std::vector<double> gains;
    std::vector<double> losses;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const double change = bars[i].close - bars[i - 1].close;
        gains.push_back(std::max(change, 0.0));
        losses.push_back(std::max(-change, 0.0));
    }
    if (period <= 0 || static_cast<int>(gains.size()) < period) return 50.0;
    const double gain = wilder(gains, period);
    const double loss = wilder(losses, period);
    if (gain + loss <= 0.0) return 50.0;
    return 100.0 * gain / (gain + loss);
}

double choppinessOf(const std::deque<Bar>& bars, int period) {
    if (period <= 1 || static_cast<int>(bars.size()) < period + 1) return 50.0;
    double sumTr = 0.0;
    double high = bars.back().high;
    double low = bars.back().low;
    for (std::size_t i = bars.size() - period; i < bars.size(); ++i) {
        sumTr += trueRange(bars[i], bars[i - 1]);
        high = std::max(high, bars[i].high);
        low = std::min(low, bars[i].low);
    }
    if (!(high > low) || !(sumTr > 0.0)) return 50.0;
    return 100.0 * std::log10(sumTr / (high - low)) / std::log10(static_cast<double>(period));
}

double smaOf(const std::deque<Bar>& bars, int period, int barsAgo) {
    if (period <= 0 || static_cast<int>(bars.size()) < period + barsAgo) return 0.0;
    double sum = 0.0;
    const std::size_t end = bars.size() - barsAgo;
    for (std::size_t i = end - period; i < end; ++i) sum += bars[i].close;
    return sum / period;
}

double emaOf(const std::deque<Bar>& bars, int period) {
    if (period <= 0 || bars.empty()) return 0.0;
    const double k = 2.0 / (period + 1.0);
    double ema = bars.front().close;
    for (const Bar& b : bars) ema = b.close * k + ema * (1.0 - k);
    return ema;
}

RegimeRead readRegime(const std::deque<Bar>& oneMinuteBars, const RegimeConfig& config) {
    RegimeRead r;
    const std::deque<Bar> bars = aggregateBars(oneMinuteBars, config.barMinutes);
    const int lookback = std::max(2, config.lookbackBars);
    if (static_cast<int>(bars.size()) < std::max(kMinBars, lookback)) {
        r.why = "needs " + std::to_string(std::max(kMinBars, lookback)) + " bars of " +
                std::to_string(config.barMinutes) + " min, has " + std::to_string(bars.size());
        return r;
    }
    r.atr = atrOf(bars, kPeriod);
    r.adx = adxOf(bars, kPeriod);
    r.rsi = rsiOf(bars, kPeriod);
    r.choppiness = choppinessOf(bars, kPeriod);
    r.emaFast = emaOf(bars, 8);
    r.emaSlow = emaOf(bars, 21);
    r.sma = smaOf(bars, kBollingerPeriod);
    r.atrMinute = atrOf(oneMinuteBars, kPeriod);

    const auto deviationOf = [](const std::deque<Bar>& from, double sma) {
        double squares = 0.0;
        for (std::size_t i = from.size() - kBollingerPeriod; i < from.size(); ++i) {
            squares += (from[i].close - sma) * (from[i].close - sma);
        }
        return std::sqrt(squares / kBollingerPeriod);
    };
    const double deviation = deviationOf(bars, r.sma);
    const double smaMinute = smaOf(oneMinuteBars, kBollingerPeriod);
    const double deviationMinute = deviationOf(oneMinuteBars, smaMinute);
    r.bollingerUpper = smaMinute + 2.0 * deviationMinute;
    r.bollingerLower = smaMinute - 2.0 * deviationMinute;

    double ticks = 0.0;
    for (std::size_t i = bars.size() - lookback; i < bars.size(); ++i) ticks += bars[i].tickVolume;
    r.recentHigh = oneMinuteBars.back().high;
    r.recentLow = oneMinuteBars.back().low;
    const std::size_t minutes = std::min(oneMinuteBars.size(), static_cast<std::size_t>(std::max(1, config.rangeMinutes)));
    for (std::size_t i = oneMinuteBars.size() - minutes; i < oneMinuteBars.size(); ++i) {
        r.recentHigh = std::max(r.recentHigh, oneMinuteBars[i].high);
        r.recentLow = std::min(r.recentLow, oneMinuteBars[i].low);
    }
    if (ticks > 0.0) r.volumeRatio = bars.back().tickVolume / (ticks / lookback);

    const double close = bars.back().close;
    const double slope = r.sma - smaOf(bars, kBollingerPeriod, 5);
    r.trendVotes = (r.adx >= config.adxTrendMin ? 1 : 0) + (r.choppiness < config.chopRangeMin ? 1 : 0) +
                   (std::fabs(r.emaFast - r.emaSlow) >= config.emaGapAtr * r.atr ? 1 : 0) +
                   (std::fabs(slope) >= config.smaSlopeAtr * r.atr ? 1 : 0) +
                   (r.rsi < config.rsiLow || r.rsi > config.rsiHigh ? 1 : 0) +
                   (close > r.sma + 2.0 * deviation || close < r.sma - 2.0 * deviation ? 1 : 0) +
                   (r.volumeRatio >= config.volumeSpike ? 1 : 0);
    r.regime = r.trendVotes > config.maxTrendVotes ? MarketRegime::Trending : MarketRegime::Ranging;

    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s: %d trend signals of 7 (ADX %.1f, Chop %.1f, RSI %.1f, ATR %.2f, ticks x%.1f)",
                  r.regime == MarketRegime::Trending ? "trending" : "ranging", r.trendVotes, r.adx, r.choppiness, r.rsi,
                  r.atr, r.volumeRatio);
    r.why = buf;
    return r;
}

RegimeRange computeRegimeRange(const RegimeRead& read, double mid, const ExternalLevels& levels, double fillsLow,
                               double fillsHigh, const RegimeConfig& config, double chosenGap) {
    // Choppy: the whole high-to-low is the zone. Calmer: closer to the Bollinger Bands.
    const double choppy = std::clamp((read.choppiness - 38.2) / (61.8 - 38.2), 0.0, 1.0);
    double high = choppy * read.recentHigh + (1.0 - choppy) * read.bollingerUpper;
    double low = choppy * read.recentLow + (1.0 - choppy) * read.bollingerLower;

    // Walls near enough to matter: the orders go beyond them, not in front of them.
    const double reach = config.maxGap / 2.0;
    for (const double wall : {levels.askWall, levels.callWall}) {
        if (wall > mid && wall <= mid + reach) high = std::max(high, wall);
    }
    for (const double wall : {levels.bidWall, levels.putWall}) {
        if (wall > 0.0 && wall < mid && wall >= mid - reach) low = std::min(low, wall);
    }

    high = std::max(high, mid);
    low = std::min(low, mid);
    if (fillsHigh > 0.0) high = std::max(high, fillsHigh);
    if (fillsLow > 0.0) low = std::min(low, fillsLow);

    // The buffer grows with volatility, trend strength and ticks; RSI leans it to the side the price presses.
    const double buffer = std::max(0.0, config.bufferAtr) * read.atrMinute * (1.0 + read.adx / 100.0) *
                          std::clamp(read.volumeRatio, 1.0, 1.5);
    const double lean = std::clamp((read.rsi - 50.0) / 50.0, -0.5, 0.5);

    RegimeRange out;
    out.low = low;
    out.high = high;
    out.buyStop = high + buffer * (1.0 + lean);
    out.sellStop = low - buffer * (1.0 - lean);

    const double gap = chosenGap > 0.0 && !(fillsHigh > 0.0) ? chosenGap : out.buyStop - out.sellStop;
    if (gap != out.buyStop - out.sellStop || gap < config.minGap || gap > config.maxGap) {
        const double half = std::clamp(gap, config.minGap, config.maxGap) / 2.0;
        const double center = (out.buyStop + out.sellStop) / 2.0;
        out.buyStop = center + half;
        out.sellStop = center - half;
        out.clamped = true;
    }

    // Never past the price: keep the distance, move both.
    if (out.buyStop < mid + config.minDistance) {
        const double shift = mid + config.minDistance - out.buyStop;
        out.buyStop += shift;
        out.sellStop += shift;
    } else if (out.sellStop > mid - config.minDistance) {
        const double shift = out.sellStop - (mid - config.minDistance);
        out.buyStop -= shift;
        out.sellStop -= shift;
    }
    out.high = std::min(out.high, out.buyStop);
    out.low = std::max(out.low, out.sellStop);
    return out;
}

}  // namespace muuk::core
