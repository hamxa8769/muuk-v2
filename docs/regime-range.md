# Regime-range strategy

A new strategy mode in the existing bot: same engine, FIX/MT5 bridge, targets and
safety code. It is off until `connection.regime.enabled` is true. There is no
Settings page for it yet: it is set in the config file.

```json
"connection": {
  "regime": {
    "enabled": true,
    "requireFeed": true,
    "feedPort": 5310,
    "minGap": 2, "maxGap": 15,
    "maxPositions": 8, "shiftAfterFills": 3, "stepAtr": 2.0
  }
}
```

All fields and defaults are in `RegimeConfig` (`engine/core/include/muuk/core/regime_range.hpp`).

## How each rule is met

| Rule | What the bot does |
|---|---|
| 8 positions at most, lock at the 9th | The 9th is never opened. A stop order for the net lots waits at its line; on a fill, buys = sells and the loss is frozen. Nothing new opens until it's resolved: **Close range**, break-even close, or *Close it after (s)*. |
| Regime check before each cycle | `readRegime` on 5-minute bars made from the one-minute ones. Seven trend signals: ADX, Choppiness, EMA 8/21 gap, SMA 20 slope, RSI, close outside Bollinger, tick-volume spike. More than 2 = trending. |
| Range: orders outside it | Sell Stop below the range, Buy Stop above it, the price always between them. |
| Steps of 3 trades | Trades 1 to 3 share the first two lines. At the 3rd fill both orders move out to a wider step (gap + 2 x the one-minute ATR, at least $1, at most $15) around the same middle, so the range has to break first. Again at the 6th. The 9th is the lock. |
| Evaluate on every tick | The range is worked out on every tick. An order moves only for at least `moveMinPoints` ($1), at most once per `moveMinSeconds` (2 s), as brokers limit order changes. Orders move before the first fill and from the 4th on, not in between. |
| Data, no guesswork | See below. |
| $2 to $15 (price) | The distance between Buy Stop and Sell Stop is clamped to it; within it, the market or the brain decides. |
| Stop orders only | Entries and the lock are Buy Stop / Sell Stop. **Closing a position still happens at market**: there is no other way to close one on time. |

### What goes into the range

- **High and low** of the last 20 one-minute bars, blended with their **Bollinger Bands** by **Choppiness** (choppy: the whole high to low; calm: closer to the bands).
- **CME order book and option chain:** the largest resting bid and ask and the strikes with the most open interest. A wall within half of `maxGap` of the price pushes the line beyond it; they only ever widen the range.
- **Buffer beyond the range:** `bufferAtr` x **ATR**, larger with **ADX** and **tick volume**, leaning to the side **RSI** presses.
- **SMA and EMA** count in the regime check.

## Architecture: the external feeds

```
 CME source (vendor API) --> feeder program --> 127.0.0.1:5310 --> ExternalFeed --> LiveRunner
 MT5 / cTrader quotes and bars ------------------------------------------------------^
```

A feeder is a small separate program, in any language, so a vendor's outage or a
crash can't take the bot down. It sends JSON lines:

```json
{"t":"book","bids":[[4369.9,10],[4365.0,500]],"asks":[[4370.1,12],[4380.0,700]]}
{"t":"options","strikes":[[4300,50,900],[4400,800,30]]}
```

Book and strikes are in the **future's** prices. The bot converts them with the
gap between the future's mid and the broker's mid at that moment.

**No real CME feed is connected yet.** It needs a paid data subscription and a
feeder for it. `tools/feeds/sim_feed.py` sends made-up numbers to test the
plumbing only: its walls mean nothing.

## Edge cases

| Case | What happens |
|---|---|
| Feed disconnected or stale (book over 5 s, options over 15 min) | No new cycle. Pending orders stay at the last good lines. Open positions are still managed (target, stop loss, lock). It resumes by itself when fresh data arrives. |
| Feed stale at the 4th fill | The rule still holds: the shift is worked out from the bars alone. |
| `requireFeed: false` | Trades on the bars alone: for tests only, it isn't the full rule. |
| Bad feed message (crossed or empty book, broken JSON) | Thrown out; the last good data ages as usual. |
| Too few bars | No new cycle. |
| Trending during a basket | The check is for new cycles: the running basket carries on. |
| Lock when the price is already past the line | A stop just beyond the price (`minDistance`). |
| Broker refuses the lock stop | Tried again further away. After 5 refusals the bot **doesn't** go to market: it reports that the basket is NOT locked, and it's your call. |
| Fast move through a lock stop | It fills with slippage, or the price moves away and it stays unfilled. With stop orders only, a lock can't be guaranteed the way a market order is. |
| Margin guard wants a lock now | Also a stop order, for the same reason. |
| An order fills while being moved | That fill is the trade: its replacement is cancelled and the engine counts it. No move happens with the price within 2 x `minDistance` or 2 spreads of a line. |
| Escape from a lock | Off in this mode: its re-lock trades at market. |

## Testing it in a demo

Demo account only, with this copy's own MT5 (`docs/mt5.md`, "This copy's own MT5").

1. **Config:** a new `*.local.json` with `"environment": "demo"`, the smallest lot, and the block above.
2. **Feed off:** connect. The log must say `[regime] no new cycle: the CME order book or option chain is stale...`, with no order at the broker.
3. **Feed on:** `python tools/feeds/sim_feed.py --mid <gold future price> --port 5310`. Once the market is ranging, the log shows `[regime] ranging...; range ... Sell Stop ... Buy Stop ...`.
4. **Trending:** in a clear trend the log must say `no new cycle: trending` and no order goes out.
5. **Gap limits:** in every `[regime]` line, Buy Stop minus Sell Stop is between 2 and 15.
6. **4-trade rule:** for a quick test set `"minGap": 2, "maxGap": 6` in the demo config. Check:
   - the first 4 fills are at the first two lines;
   - right after the 4th, the log has `4 trades in this range: new range ...`, with both new orders outside the highest and lowest of those fills;
   - the 5th fill is at a new line, never an old one.
7. **Lock:** with the small test gaps, let it reach 8 positions. Check:
   - never 9 positions in MT5;
   - no pending entry order remains, just one lock stop for the net lots;
   - once it fills, buys = sells and the net stops moving;
   - no new order until you press **Close range**.
8. **Feed drop:** stop the feeder with orders pending. After 5 s they stop moving, and no new cycle starts after a basket closes. Start it again: it resumes.

**What to compare:** MT5's History tab against the bot's log.

| Metric | Expected |
|---|---|
| Positions open at once | 8 at most |
| Market orders that opened a position | 0 (in MT5, Type is buy stop / sell stop) |
| Fills 5+ at old lines | 0 |
| Buy Stop minus Sell Stop | always 2 to 15 (or your test values) |
| Order changes per minute | 30 at most with the defaults, and no broker refusals for too many |

Automated checks are in `engine/core/tests/test_regime_range.cpp` and
`engine/ctrader_fix/tests/test_regime_live.cpp`; they run with `ctest`.

## The brain

`connection.regime.brainFile` turns it on (`engine/core/src/market_brain.cpp`).

- **What it learns:** for 81 kinds of market (volatility, trend, chop, session) it replays the strategy on past one-minute bars with 5 gaps (2 to 8 x ATR, within $2 to $15), and keeps the gap with the best mean less one standard error. Where none beats not trading, or there are under 20 samples, the answer is no trade.
- **The test:** walking forward. The bars after the first 30% are cut into 11 periods; before each, a brain learns only from the bars before it, then trades that period. A new brain counts only with a profit over all of them, more than a fixed 3 x ATR gap and the brain in use, and no deeper drawdown than the fixed gap.
- **Improving by itself:** the bot keeps every bar it sees in `bars_live.csv`, and after each 1,440 new bars learns again on a low-priority thread. Every attempt goes to `training_report.txt`.
- **Its limits:** it chooses only the gap and whether to trade. It can't change lots, limits, the lock or code.
- **`brainRequired`:** true (default): no new cycle until a brain has passed, and none where it says no. false: where the brain says yes its gap is used; otherwise the worked-out gap.
- **What it isn't:** the replay reads one-minute bars, so the inside of a bar is a harder guess, slippage isn't in it, and its results come from one market period.

Teach it by hand: `Muuk-Testing-Brain <history folder> <brain file>`. The history is MT5's export format (`MQL5\Files\MuukHistory`).

## Walk-forward result

`Muuk-Testing-Brain walkforward <history folder>`: in each of 11 periods (Nov 2023 to Nov 2026) the brain learned only from the bars before it. Per 0.01 lot start, spread in, slippage and commission out:

| Rule | Periods up | Locks | Total | Drawdown (about) |
|---|---|---|---|---|
| Fixed 3 x ATR, x2, 8 levels | 2 of 11 | 88 | -22,907 | 25,600 |
| Brain, x2, 8 levels | 7 of 11 | 33 | +945 | 15,242 |
| Brain, x1, 8 levels | 5 of 11 | 5 | +31 | 461 |

The brain's choices cut the loss, but about break-even with a large drawdown is no proof of profit.

## Defaults, and cent or standard accounts

`engine/ctrader_fix/config.regime.example.json` is the default config. Settings has the same controls, under **Regime strategy**.

- **The same on both:** the gap, levels and lots as numbers. A $ of gold's price is a $ on any account.
- **In the account's own money:** P/L, targets, the lock's cost and the limits. MT5 says which account it is; on cTrader it's the account type in Settings.
- **The same lot isn't the same risk:** 0.01 lot makes 1 USC per $1 move on a cent account, $1 on a standard one. So `maxLockRiskPercent` (25) stops a new cycle when a full lock would cost too much of this account's balance.

## The risk in these numbers

Lots doubling from 0.01, 8 trades alternating between two lines, the lock filling at the 9th level's line, before costs:

| Gap | Cent account | Standard account |
|---|---|---|
| $2 | -340 USC ($3.40) | -$340 |
| $15 | -2,550 USC ($25.50) | -$2,550 |

(The last side, 1.70 lots, a whole gap wrong.) A brain has passed no test yet: **no backtest here shows this mode making money reliably**. Its result changes a lot with the bars it learned from.
