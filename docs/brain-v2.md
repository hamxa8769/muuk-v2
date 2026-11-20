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
test was omitted — `moveRange` does not exist in this tree.

### Live wiring (LiveRunner)

`BrainKeeperV2` is wired into `LiveRunner` (`engine/ctrader_fix`):
the runner owns a `unique_ptr<BrainKeeperV2>` and calls it on every new
cycle, in `LiveRunner::prepareNextCycle` before the engine is
reconfigured. `BrainKeeperV2::decideForCycle()` returns the decision
plus the situation it was based on, so the runner can report it and
feed the outcome back later.

Per cycle the runner passes: the 1-minute bars, the outcome of the last
completed basket (`-1` if it lost or locked, `+1` if it won, `0` if
there is none yet), the live spread, and the median spread of the last
120 quotes. The spread and its median must differ — the brain vetoes
a cycle when the spread is more than twice its median — so the runner
keeps a small deque of recent spreads and computes the median itself.
The daily P/L and the account balance go in as `BrainAccountState`
(balance comes from the broker when known, else the config's
`accountBalance`).

Policy -> strategy mapping (applied in `engineConfigLocked()`, after the
configured values, only when the brain decides to trade):

| Brain policy | Strategy config |
|---|---|
| `policy.gap` (gap in points) | range width: the range is re-centered on the live mid, `[mid - gap/2, mid + gap/2]` |
| `policy.lotMultiplier` | `lotProgression.multiplier` (geometric progression) |
| `policy.maxLevels` | `riskLimits.maxOrders` (deepest level the range may reach) |
| `policy.exitFactor` | `lastLotTarget = true`, `lastLotTargetMove = max(0.01, gap/2 x exitFactor)` — the basket's money target is the last lot's size times half the gap times the exit factor |

If the brain says no-trade, the cycle is blocked: `entryWait_` is set
to `"brain: " + why`, the reason is logged once, and no range opens.
The runner retries on the next quote, so a veto is a pause, not a
stop. The veto also fires when the brain is untrained or the situation
cannot be read.

When a basket closes, `maybeFinalizeBasket` reports it to the keeper
(`observeBasket` with the stored situation and policy, the basket net,
and whether it locked). The keeper only ever vetoes a cell — it never
invents a policy — and the call is guarded so learning can never break
basket finalization.

Closed 1-minute bars are fed to the keeper (`addBars`) as they arrive
so its background re-training can run: after 1,440 new bars it
re-trains and adopts the new brain only if the walk-forward test
passes, writing `training_report_v2.txt` next to the brain file.

#### Real-money guard (non-negotiable)

Brain-driven cycles are refused on a real account — `connection.environment
== "live"` or the broker reporting a real account — unless the config
explicitly sets `brain.allowRealAccount: true`. The refusal is logged
once and the cycle opens on the configured strategy instead. The
default is `false`: the validated brain has NOT passed its strict
gates (48 locks, held-out last 15% -$1,102), so it is demo-only until
those gates pass.

#### Configuration

```json
"brain": {
  "enabled": true,
  "brainFile": "brain_v2.txt",
  "allowRealAccount": false
}
```

- `enabled` (default `true`): turn the wiring off and the runner
  behaves exactly as before.
- `brainFile` (default `"brain_v2.txt"`): resolved as an absolute path
  if given one, else next to the runner's state file, else the working
  directory. If the file is missing or the brain inside it is
  untrained, the runner logs once and falls back to the configured
  strategy — it never crashes and never starts the keeper's background
  learning over a brain that isn't there.
- `allowRealAccount` (default `false`): see the guard above.

`LiveSnapshot.brainStatus` reports the keeper's status line
(`"off"` when the brain is disabled, otherwise the keeper's own
status: trained/untrained, learned bars, online vetoes), so the UI or
a log can show which strategy is actually driving the runner.

#### Money scaling (cent accounts)

The brain's money figures are replay units: standard-account dollars,
where 1 lot earns $100 per $1.00 of price move, starting from a 0.01
lot. The live account counts in its own currency, so the runner
converts every money figure it shows the brain into replay units with
one factor — `100 / moneyPerPointPerLot`, where `moneyPerPointPerLot`
is the adopted symbol's `tickValue / tickSize` (see
`LiveRunner::brainMoneyScaleLocked`):

| Account | tick value | money per point per lot | 1 unit of account money = |
|---|---|---|---|
| Standard USD | 1.0 per 0.01 | 100 | $1.00 replay |
| RoboForex-style ProCent | 100 USC per 0.01 | 10,000 | $0.01 replay (0.01 lot x $1 = 100 USC) |
| 3-digit XAUUSDc | 0.1 per 0.001 | 100 | $1.00 replay (that symbol's lot is 1 oz) |

The factor carries the currency and the contract size together, so it
is right for both cent shapes above — it is the number the gateway
test already pins (`moneyPerPointPerLot` 10,000 on ProCent, 0.01 lot x
$1 = 100 USC gross). It is applied to the day's realized P/L, the
account balance, and every basket net fed back to the keeper's online
learning. Gap, spread, lots and levels are not money, so they are
never scaled; the exit target is a price move, and the engine turns
it into money with the same adopted symbol, so it comes out right in
account money on its own.

Two honest limits. First, the money-dependent caps (`maxDailyLoss`,
`maxDrawdown`, `maxLockCostPctOfBalance`) are all off by default
(0.0), so with the keeper's default config the scaling changes no
decision today — it is there so the caps are correct when they are
turned on. Second, the runner tracks the day's realized P/L but no
equity peak, so `drawdown` is always 0.0 and the brain's `maxDrawdown`
cap is inert live; the runner's own daily-loss limit is the live
equivalent.

#### MT5

The wiring is platform-agnostic: the keeper only sees 1-minute bars
and spreads, and its history format is MT5's export format. On the
MT5 path the bars and the spread come from the bridge's FIX quotes
exactly as on cTrader — `onQuoteMessage` feeds the keeper and records
the spread — and the on-connect history seeds the keeper through
`addHistoryBars`:

- The bridge (`MuukBridge.mq5`, `QueueBars`) sends the last 240
  finished one-minute bars on every connect; the gateway
  (`Mt5Gateway`) asks for them when a QUOTE session subscribes and
  holds them for `takeHistoryBars()`.
- The desktop app feeds them from `LiveController::tick()` into
  `runner_->addHistoryBars(bars)`.
- The CLI (`muuk-live`, `engine/ctrader_fix/cli/main.cpp`) has no
  tick, so a small feeder thread polls `takeHistoryBars()` once a
  second and calls `addHistoryBars` on the runner of the moment.
  Without this the CLI path would build its bars from live ticks
  only — 150 minutes to the 150 bars the brain needs to read the
  market, during which the brain vetoes and the configured strategy
  runs.

The real-money guard uses MT5's own word for the account:
`BrokerAccountInfo.tradeMode` comes from the bridge
(`MuukTradeMode`), and `brokerAccount_.real()` is true only when MT5
says `"real"`. The runner already refuses to trade at all when a real
MT5 account is paired with a demo config, so a real account with a
demo config never reaches the brain. The user's only configured
account is the MT5 demo, so the default behaviour is demo.

`engine/ctrader_fix/config.mt5.example.json` carries a ready MT5
setup: `platform` `"mt5"`, `environment` `"demo"`, `accountKind`
`"cent"`, and the brain section enabled with `brainFile`
`"brain_v2.txt"`. It keeps the `YOUR_MT5_*` placeholders — no real
credentials.

#### Entry points

Both ways of starting the runner go through the same wiring, because
it lives in `LiveRunner::run()` / `prepareNextCycle`, not in a
platform branch:

- CLI: `engine/ctrader_fix/cli/main.cpp` — `muuk-live --credentials
  <config.json>` loads the config with `loadLiveConfig` (which parses
  the `"brain"` section), starts an `Mt5Gateway` when `platform` is
  `"mt5"`, constructs `LiveRunner` and calls `run()`.
- Desktop app: `app/desktop/src/live_controller.cpp` —
  `LiveController::startRunner` loads the same config, ensures the
  bridge, constructs `LiveRunner`, sets the state file, and runs it
  on a thread; `tick()` feeds the MT5 history bars.

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
