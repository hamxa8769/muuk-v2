# Muuk changelog

Every Muuk update gets a patch version bump, a new tag and a new release
(app version 0.2.x ↔ release tag v2.0.x), and each release adds an entry
below. The in-app updater offers every new release, so nobody has to
download a zip by hand.

## 0.2.7 — 2026-11-25 — Brain upgrade: new trained brain, one Brain panel, in-app trainer

- The update button is smart now: it checks GitHub on startup in the
  background and shows "✓ Up to date" when this is the latest release, a
  gold "⬆ Update available: v2.0.7" with the release tag when a newer one
  is out, "Checking…" while a check runs, and "Check failed — try again"
  when GitHub cannot be reached. A failed check never hides an update
  that was already found, and a second click never starts a duplicate
  check.
- The app version (0.2.x) is shown next to the MUUK brand in the header.
- This file: the version history, kept with every release.
- The brain file is found now: a relative `brain_v2.txt` is searched next to the config file first (where the
  packager and the updater put it), then next to the state file, then the working directory. This fixes
  "no trained brain" on installs where the file was at the app root all along. A missing brain is retried on
  every cycle decision (a file dropped in later is picked up without a restart), a loaded brain is re-read when
  its file changes, and the Range panel has a refresh button next to the Brain line for a manual re-read. On
  failure the log lists every path it looked in.
- Settings has a Keys section (Gemini / FRED / Finnhub API keys): stored only in the local config file, never
  in the repo, never logged, shown masked (last 4 characters).
- The range shift is a trained brain parameter now: the trainer searches shiftAfterFills {2,3,4} x stepAtr
  {1.0,2.0} and writes the winning shift into the brain file; a loaded brain's shift replaces the config shift
  for its cycles (shown on the Brain line as "shift 3x2.0" and logged once per cycle). A brain file written
  before this still loads and falls back to the config shift. Winner on the same 1.5M bars with the ER mode 2
  gates: 3 fills x 2.0 ATR (+$251.28, 2 locks, held-out +$149.83 / 1 lock) -- the shift the gates passed with.
- Step 4 -- one Brain Settings panel: the Settings page opens with the Brain panel holding all of the bot's
  core settings (live brain status + refresh, risk appetite, hard caps, ER gate, brain file, Smart ranges, and
  the Keys). The old Strategy table is now the Fallback, used only when the brain is off or has no trained
  file. Seven options are removed or merged: the trend filter (trend is a brain input now, Kaufman Efficiency
  Ratio), first order with the trend (Both stays -- a forced first side lost in backtest), target move = half
  the range (the brain's exit decides), auto profit target (fixed / last-lot target only), max basket loss
  (merged into the stop loss), lot growth presets (replaced by the risk knob), and Range from ATR stays in the
  Fallback (it only ever set the fallback width). Old config files still load: the removed keys are migrated
  (stop loss off since Step 6b, max lot 1.00, daily loss 2% of the balance, Balanced) and never error.
- Step 4 -- risk appetite is one knob: Cautious (worst lock $75, at most 4 levels, daily loss 1% of the
  balance), Balanced (worst lock $150, 6 levels, 2% -- the default), Aggressive (worst lock $300, 6 levels,
  3%). The hard caps are enforced, not labels: the daily loss limit stops trading for the day when hit, no
  single lot is ever above 1.00, and a basket closes at the stop loss when one is set (off by default
  since Step 6b).
- Step 4 -- a restarted basket is picked up by its recorded start side now, not by the "First order" dropdown:
  a basket that started Sell-first is adopted even when the settings say Buy first.
- Step 4 also fixed a staging-only compile break: the Keys block read the settings variable after the table's
  scope had closed, so the dashboard would not have compiled on Windows.
- Step 5 -- the in-app trainer: a Train button next to the Brain line (Range panel and Brain Settings panel)
  trains a new brain inside the app, in the background, on the bars the bot collected (bars_live.csv next to the
  brain file) or on a history CSV/folder given in the new "Trainer history" field (an MT5 1-minute history
  export). The bot keeps trading its current brain while a run goes; the panel shows live progress ("Training...
  walk-forward 4/11, N bars") and, when the run finishes, the gates result with the numbers (total P/L, locks,
  drawdown, periods up, held-out P/L and locks). A new brain is saved and adopted for the next cycle only when
  the strict gates pass -- a run that fails them adopts nothing and says which gate failed. A fresh install has
  almost no bars yet, so the first Train refuses with "too few bars" (a run needs at least 2,000 bars, and
  ~5,800 to learn at all) until bars accumulate or a history file is given.
- Step 5 -- the trainer validates the brain under the caps it will trade under: the basket stop loss is
  modelled in the replay (a basket stops out at -$stop instead of running on to the lock), using the panel's stop
  loss converted into replay units. The risk caps still use the worst-case lock cost, not the stop -- fully
  exploiting the stop in the caps is Step 6 work.
- Step 5 -- the brain's background re-training now adopts only on the strict gates too (it used to adopt on a
  walk-forward-only "better"). The keeper is built even when no trained brain exists yet, so the bars the bot
  sees accumulate from the first connect and Train always has a keeper to train with; the Brain line still
  reports "no trained brain" until one passes its test.
- Step 6 -- retrain under the final config (ER mode 2, shift 3 fills x 2.0 ATR, $50 stop loss, first side
  Both, Balanced caps): the retrain did NOT pass the strict gates, so that retrain shipped no brain --
  the shipped brain stayed the v0.2.5 one until Step 6b below. The $50 stop is the cause: it turns
  cycles that dip deep and recover into -$50 closes (walk-forward +$31.89, 0 locks, 6 of 11 periods up;
  held-out -$191.43). Diagnostics on the same 1.5M bars: stop $25 (-$12.55, held-out +$68.33), stop $100
  (-$432.75, held-out -$363.03) -- no stop width passes; only the no-stop brain passes. The stop and the
  strategy's edge (deep-dipping recoveries) are in tension -- the user decided (2026-11-25): the stop goes
  (Step 6b).
- Step 6 -- the risk caps now bound the effective worst case: with a stop loss the worst case is the stop, not
  the no-stop lock cost (the stop is a hard, enforced, audited live cap and the replay models it exactly).
  Proved inert for the learned brain: the retrain's numbers are identical with conservative and stop-aware
  caps -- the learned policies never hit the $150 cap, so the caps were never the constraint.
- Step 6 -- the brain file now records the ER mode and stop loss it trained under (header fields; a brain file
  written before this still loads and falls back to the panel's ER mode). A loaded brain's recorded ER mode
  wins live, like its trained shift, and the Brain line shows it (e.g. "erMode 2"). The panel's ER gate default
  stays Off (0) -- it is the no-brain fallback; the shipped brain (Step 6b) records erMode 2, so the override
  is what applies live. The stop loss is not overridden from the brain: the panel governs the live risk control.
- Step 6b -- the $50 stop is gone (user decision, 2026-11-25): the stop loss defaults to OFF (0) and no stop is
  forced onto an old config file any more -- an old file with no stop stays off, an explicit stop is kept as
  written, and a max-basket-loss still migrates into the stop when the file sets one. The brain's $150 lock cap
  (Balanced) remains the worst-case bound, and the stop-loss feature itself stays (engine enforcement + replay
  modelling) for anyone who sets one. The retrain WITHOUT a stop passes the strict gates: +$251.28, 2 locks,
  drawdown $190.92, 7 of 11 periods up, held-out +$149.83 / 1 lock -- the Step 1 mode 2 / Step 2 numbers,
  reproduced exactly. The new brain ships in this release (erMode 2 and shift 3 fills x 2.0 ATR recorded in its
  header; same 4 tradable cells as before -- 54, 59, 62, 71, all Both first side). The panel's ER gate default
  stays Off -- it is only the no-brain fallback; the shipped brain's recorded erMode 2 is what applies live.
- The updater installs the release brain now: on update it reads only the header of the installed brain and
  of the release brain (trainedUntilMs, ER mode) and installs the release brain when the installed one is
  older, or when it predates the Step 6 header (no recorded ER mode -- the v0.2.6 brain). A locally-trained
  brain that is newer than the release is never overwritten -- the in-app trainer's brain wins, and so does a
  same-age brain with a recorded ER mode. An update from v0.2.6 therefore gets the new trained brain, not
  just the new code.

## 0.2.6 — 2026-11-24 — Phase 1 of the hi-fi brain

- The brain keeps a journal: every brain-driven basket close and every
  brain veto appends one JSON line to
  `muuk_history/<config>.brain_journal.jsonl` next to the account config
  (best-effort, never blocks the live loop, no credentials). The journal
  is the memory the later phases build on.
- The dashboard Range panel shows the brain thinking: the active cell and
  its policy (gap / lot multiplier / levels / exit) and "Brain today"
  (cycles, wins, locks).
- Trading behaviour unchanged.

## 0.2.5 — 2026-11-24 — First trained Brain v2

- The trained Brain v2 ships as `brain_v2.txt`: an 81-cell table learned
  from 1,500,000 one-minute bars. Strict gates PASSED (+$439.30, 9 locks,
  drawdown $336.84, 5 of 11 periods up, held-out +$120.29 with 4 locks);
  only 4 of the 81 cells trade.
- Live "Smart ranges (chart)" wiring, default OFF: regime-range placement
  and the trend-filter bypass when it is on.
- Dashboard Brain/Chart status lines.

## 0.2.4 — 2026-11-23 — Stale symbol fix

- Fixed the live bug where a stale cTrader-era symbolId made the bot
  blind to its own MT5 positions and orders: the MT5 path always uses
  the SecurityList now, and saving an MT5 account always clears the
  stored symbolId.
- The updater normalizes release tags (v2.x.y) to app versions (0.x.y)
  before comparing, so the running version is never offered to itself.

## 0.2.3 — 2026-11-23 — Live range shift, cent labels

- After every 2 fills the live range widens by $1 around the same
  center, capped at $15.
- Cent-account stats (costs line, commission tooltip, COMMISSION PAID,
  period commission) now use cents; notional stays in dollars (gold face
  value, not account money).

## 0.2.2 — 2026-11-23 — Updater batch fix

- The windowless update batch waits with `ping` instead of `timeout`
  (which exits immediately without a console) and logs to
  `%TEMP%\MuukUpdate\update.log`, so the app actually restarts after
  an update instead of closing and staying closed.

## 0.2.1 — 2026-11-21 — In-app updater

- First release with a self-updater: "Check for updates" asks GitHub
  Releases for a newer Muuk and installs it over the app folder,
  keeping the user's own config files (`*.local.json`).
