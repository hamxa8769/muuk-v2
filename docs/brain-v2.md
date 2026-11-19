# Brain v2 — the super-upgraded brain (C++)

Status: **implemented in C++, validated by walk-forward — see
`BRAIN_UPGRADE.md` for the numbers and the gate verdict.** Demo-only until
the gates pass. No real money, no `force` without the user's written approval.

## What v1 was

A tabular lookup, 81 cells (volatility x trend x chop x session). Each cell
chose one thing only: the gap (2, 3, 4, 6 or 8 x ATR) or no trade. It could
not change lots, levels or exits, the replay charged only the spread (no
slippage, no commission), and the brain in use (`demo/muuk_brain/brain.json`)
was `forced: true` — put in by hand after failing its test.

## What v2 is

Same 81-cell table, so every live decision is still explainable as
"state X -> policy Y, N samples, mean +Z". What a cell learns now is a
whole **policy**:

| Field | Choices | Meaning |
|---|---|---|
| Gap | 2, 3, 4, 6, 8 x ATR ($2–$15) | as v1 |
| Lot multiplier | 1.0, 1.25, 1.5, 2.0 | each level's lot x this |
| Max levels | 3, 4, 6 | positions before the lock |
| Exit factor | 0.5, 1.0 | target = last lot x gap/2 x this (0.5 = exit earlier) |

The eight candidate policies stage B chooses between, safest first:

`1.0/3`, `1.0/4`, `1.25/4/exit0.5`, `1.25/4`, `1.5/6/exit0.5`, `1.0/6`,
`1.5/3`, `2.0/4`. A riskier policy is picked only if it scores clearly
better (mean − 1 SE, +0.05 margin) than the safer one already picked.
`2.0/8` — the v1 lot rule — is never a v2 policy: the lock-cost caps
would not allow it anyway.

### How it learns

Two stages, both replaying the strategy on past one-minute bars:

1. **Stage A — the gap.** Replayed under the v1 lot rule, because that is
   where this strategy's edge actually lives: the lot growth on winning
   cycles. (Learning the gap under a flat policy first was tried — it
   found only 2 tradable cells in 60,000 bars, because flat lots hide
   the edge. The gap is learned the v1 way; stage B then makes the
   policy safe.)
2. **Stage B — the policy.** At the gap stage A picked, each candidate
   policy is replayed. A policy is rejected if its shrunk lock rate
   `(locks+1)/(samples+10)` is over `maxLockRate` (default 5%). The
   winner is the best score (mean − 1 standard error), with the safer
   policy winning ties.

The replay now charges the real costs: spread + **slippage** (default
$0.05 per lot per fill) + **commission** (default $0). v1 charged the
spread only.

### What v2 sees that v1 did not

These are **gates** on the 81-cell decision, not new cell dimensions (a
full cross-product would need far more history than exists to fill):

- **ATR trend** — volatility rising, flat or falling vs 3 hours ago.
- **Trend direction** — EMA8 vs EMA21, when the gap is clear; with ADX
  bucket 2 (strong trend) and a clear direction, no new cycle.
- **RSI zone, Bollinger position, tick-volume** — a volume spike needs a
  stronger cell (score ≥ 0.25) to trade.
- **Recent basket outcome** — after a lost or locked basket, a cell
  needs score ≥ 0.25 to trade again. This is a **one-decision cooldown**:
  a refused decision consumes it, so one loss can never stall trading
  for a whole period.
- **Spread vs its median** — current spread over 2x its median: no trade.

### Hard caps — the brain proposes, risk disposes

`BrainRiskCaps` (in `BrainV2Config::caps`), applied in `applyRiskCaps()`
to every decision, in the trainer and in live use. The brain's policy is
only ever lowered, never raised:

- **Max lock cost** $150 per 0.01 lot start (also as % of balance if set):
  levels are cut, then the multiplier, until the worst-case lock cost
  fits — or the trade is refused.
- **Max levels** 6, **max single lot** 16x.
- **Max daily loss, max drawdown, max spread**: breached → no new cycle.

The worst-case lock cost is computed exactly (`estimatedLockCost()`):
alternating fills to the lock, both starting sides, the worse one. At a
$4 gap the v1 `2.0/8` policy costs **$680**; a `1.0/3` policy costs **$8**.

### Online learning — learns from its own baskets

`observeBasketOutcome()` updates only the cell that was traded, with a
capped weight (one basket moves a cell's mean by at most ~1/(samples+1),
and older outcomes fade past 500 samples). It can only **veto**: if the
updated score is no longer positive, or the lock rate is over the limit,
the cell becomes no-trade until a full re-train. It never changes a gap,
lot or level on its own. Only `trainBrainV2()` can change a policy, and
only if it passes walk-forward.

### Files

| File | What it is |
|---|---|
| `engine/core/include/muuk/core/market_brain_v2.hpp` | v2 types, config, caps, training |
| `engine/core/src/market_brain_v2.cpp` | learn / decide / replay / caps / online learning |
| `engine/core/tests/test_market_brain_v2.cpp` | lock cost, slippage, caps, gates, online veto, file round-trip, learning |
| `engine/core/cli/brain_v2_main.cpp` | `muuk_brain_v2_trainer` — walk-forward validation on MT5 history |
| `engine/ctrader_fix/include/muuk/ctrader_fix/brain_keeper_v2.hpp` | live keeper: decide, observe, background re-train |
| `engine/ctrader_fix/src/brain_keeper_v2.cpp` | loads/saves `brain_v2.txt`, writes `training_report_v2.txt` |

The v1 brain files (`market_brain.*`, `regime_range.*`) were merged into
this tree from the worktree, so v1 stays available as the fallback and
the comparison in every v2 report. The v1 `StrategyEngine::moveRange`
test was omitted — `moveRange` does not exist in this tree — and the
live regime wiring into `LiveRunner` is still a separate step:
`BrainKeeperV2::decide()` is the wiring point.

### Brain file format

`brain_v2.txt`, plain text (no JSON dependency in core):

```
muuk-brain-v2 1
volLow 0.36 volHigh 0.53 trainedBars 1500000 trainedUntilMs ... forced 0
gapAtrChoices 2 3 4 6 8
0 1412 -1 1.00 3 1.00 0.00 0.00 0.00 0 0.00 0.00 0.00 0.00
...
```

One line per cell: index, samples, gap choice, lot multiplier, max
levels, exit factor, mean, sum, sum of squares, locks, lock rate, mean
lock loss, lock-loss sum, score.

## Validating it

```sh
# from the project root, after building muuk_brain_v2_trainer
muuk_brain_v2_trainer demo/muuk_brain --slippage 0.05 --out brain_v2.txt
```

It prints the walk-forward table (v2 / fixed 3xATR / v1), the held-out
last 15%, and the gate verdict, and saves `brain_v2.txt` **only** if v2
passes the adoption test (total > 0, better than fixed and v1, drawdown
no deeper than fixed). The stricter gates — drawdown ≤ 50% of v1, locks
≤ 50% of v1, ≥ 5 of 11 periods up, held-out > 0 — are printed separately
and must pass before any demo use. v1's numbers in the report are
without slippage/commission, so v2-vs-v1 is conservative for v2.

## What v2 will NOT do

- It will not guarantee profit. On the earliest 200,000 bars of the
  history (Aug 2022, a downtrend) every variant loses — v2 just loses
  the least, with the smallest drawdown. See `BRAIN_UPGRADE.md`.
- It will not trade real money, change the lock, or override a risk cap.
- It will not use a live CME feed — the plumbing exists, the feed does not.
