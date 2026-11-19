# Muuk Brain v2

Direct C++ upgrade of the Muuk XAUUSD pending-order strategy brain (user chose Direct C++ over a Python prototype, 2026-11-20).

## What changed vs v1

- Same 81-cell table (volatility x trend x chop x session). Each cell now learns a **policy** — gap + lot multiplier + max levels + exit factor — instead of only a gap.
- Hard risk caps the brain can never override: max lock cost $150 per 0.01 lot start, max single lot 16, max levels 6, never 2.0x / 8 levels.
- Online learning (`BrainKeeperV2`) can only **veto** a cell; it never invents a policy. Full re-train adopts only on a passing walk-forward test.
- New files: `engine/core/include/muuk/core/market_brain_v2.hpp`, `engine/core/src/market_brain_v2.cpp`, `engine/core/tests/test_market_brain_v2.cpp`, `engine/core/cli/brain_v2_main.cpp` (trainer), `engine/ctrader_fix/.../brain_keeper_v2.*`, `docs/brain-v2.md`.

## Honest validation (full 1,500,000 bars, slippage $0.05, per 0.01 lot start)

- v2: 2,047 cycles, total **+$1,230.65**, max drawdown **$547.95**, 48 locks, 6/11 walk-forward periods up.
- Fixed 3xATR: -$4,511.28. v1 brain: -$8,101.25, drawdown $10,447.09.
- Held-out last 15%: v2 -$1,102.16 (37 locks). Strict gates **NOT PASSED** (locks 48 > 10 allowed; held-out negative). Final brain trades in only 4 of 81 cells.
- Tests: 83 test cases, 670 assertions, all passed (Linux, system Catch2 + g++).

## Demo-only

No real-money trading. `BrainKeeperV2` is not yet wired into LiveRunner. The 73 MB history CSV and any `*.local.json` credentials are intentionally NOT in this repo.
