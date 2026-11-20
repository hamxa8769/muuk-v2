# MetaTrader 5 (cent accounts) -- run from the Muuk app

Muuk trades cTrader over FIX. Cent accounts (JustMarkets Standard Cent,
RoboForex ProCent, ...) are on MetaTrader 5, which has no FIX or trading API
for outside programs: only code running inside MT5 may trade there. So Muuk
brings its own piece for the inside, **and puts it there itself** -- you
control everything from the Muuk app, exactly as with cTrader:

```
 Muuk app (GUI) / muuk-live
   ├─ starts MetaTrader 5 (minimized), logs in, turns algo trading on,
   │  and attaches MuukBridge (built into Muuk, compiled by MT5's MetaEditor)
   ├─ MuukBridge  <── named pipe \\.\pipe\MuukUpgradeBridge_5300 ──>  bridge
   └─ LiveRunner  <── the same FIX as cTrader, 127.0.0.1:5301 ──> bridge
```

Everything works the same (range, lots, auto target, margin guards, lock,
escape, audit, history, reports). On top, MT5 gives what FIX never did:
the **exact balance, equity, margin, free margin and swap**, the account
currency (USC = cent) and the symbol's lot sizes.

## Your broker's MT5 (already open)

Muuk finds the MetaTrader 5 that is already open on this PC -- any broker's --
and uses it. No separate copy, no marker file:

1. Open your broker's MT5 and log in to your demo account (leave it open).
2. Muuk: **Settings > Account > Platform: MetaTrader 5** -- the list shows every MT5 on the PC ("(broker's MT5)", "(running)" for the open one); "auto" takes the first.
3. **Connect.** Muuk installs its bridge EA into that MT5 once (a standard EA install, compiled by MT5's own MetaEditor) and, if MT5 is already open, tells you to drag `MuukBridge` from MT5's Navigator onto the gold chart once -- it stays there afterwards, and the bridge connects as soon as it is on a chart. If MT5 is closed, Muuk starts it minimized with the bridge already on a chart.

Muuk never closes or restarts your broker's MT5 by itself. The only way it ever closes one is the **"Restart MT5 with bridge"** button (Settings > Account, or the header pill), which asks first and then closes MT5 the way its X button does and starts it again with the bridge. Everything else -- the heal that restarts a silent MT5, the automatic account switch -- only ever acts on this copy's own MT5 (below).

## This copy's own MT5

This is the upgrade copy; production runs next to it on the same PC, on a real
account. So this copy never closes or restarts an MT5 unless a `muuk_upgrade_mt5.marker`
file is next to its `terminal64.exe`, and it runs that MT5 with `/portable`, so its data stays
in its own folder. Installing the bridge EA and starting an MT5 work for any MT5 on the PC
(see "Your broker's MT5 (already open)" above). Set one up once, in a folder at the top of this drive:

1. Install MT5 into e.g. `F:\MT5Upgrade` (or copy an install's program files there).
2. Create the empty file `F:\MT5Upgrade\muuk_upgrade_mt5.marker`.
3. Run `F:\MT5Upgrade\terminal64.exe /portable` once, log in to a **demo** account, close it.

The rest of this copy differs too: the EA installs as
`MQL5\Experts\MuukUpgrade\MuukUpgradeBridge`, magic 770078, pipe
`MuukUpgradeBridge_5300`, FIX port 5301 (a config on 5200 / 5201 is refused),
preferences in `%APPDATA%\MuukUpgrade`, Windows entries named `Muuk Upgrade`.
All of it is in `muuk/ctrader_fix/instance.hpp`; don't carry those values to production.

## Use it

1. Install your broker's MetaTrader 5, open it and log in to your demo account. Leave it open --
   Muuk uses the MT5 that is already open; if it is closed, Muuk starts it.
2. Muuk: **Settings > Account > Platform: MetaTrader 5**:
   - MetaTrader 5: which MT5 (each broker installs its own; "auto" = the first);
   - Login, Password, Server: the MT5 account (empty login = the one MT5 used last);
   - Account type: **Demo** or LIVE -- must match the MT5 account: a REAL MT5
     account under a Demo config is refused before any order;
   - Save.
3. **Connect.** If MT5 is closed, Muuk starts it minimized with the bridge on a chart; if MT5 is already
   open, Muuk installs the bridge EA into it -- drag `MuukBridge` from the Navigator onto the gold chart once
   (it stays there afterwards) -- and the header shows `MT5 <login>` (green, "cent" on a cent account) and the
   bot trades.

MT5 keeps that chart, so the next time MT5 opens (by Muuk or by you) the
bridge runs by itself. With MT5 already open and no bridge on a chart, drag `MuukBridge` onto the gold chart
once, or press "Restart MT5 with bridge" (Settings > Account).

### The MT5 account

**The bot trades only the config's account.** Settings > Account > Login names it. If MT5 is logged in to any other account, the bot refuses to connect, so a live account under a demo config, or someone else's account, is never traded.

**Muuk switches its own MT5 by itself.** When its own MT5 opens on another account:
- Muuk closes MT5 (as its X button does) and opens it again, logged in to the config's account;
- it tries this twice;
- then the **MT5 accounts** list opens.

A broker MT5 is never closed for a switch: press "Restart MT5 with bridge" (Settings > Account) and MT5
opens again on the config's account.

The switch can be turned off in that list.

**The MT5 accounts list** (the *Accounts* button next to the MT5 pill in the header):
- Every account MT5 has been logged in to on this PC is remembered with its server, company and demo/real. Passwords are never remembered.
- Click one to trade it:
  - the config's login and server are set to it;
  - the Account type follows its demo/real;
  - MT5 is switched to it.
- *Add or fix an account* takes a login, password and server for one MT5 can't log in to by itself. The password goes into this config file only.

**Logging in:**
- A config that gives only the login still logs in, on the server that account was last seen on.
- The config's saved password belongs to its own login: changing the login never sends one account's password to another.
- An account without a password in the config logs in with the one MT5 saved for it.

While the bot is connecting, Muuk heals this by itself: when the bridge hasn't
connected for a minute after MT5 was started, Muuk closes its own MT5 (as its X button
does) and starts it again with the bridge -- up to 3 times, counted again once
the bridge connects. A broker MT5 is never healed this way: Muuk only says what to do
(drag the bridge onto the chart, or press "Restart MT5 with bridge").

The bridge's chart is the account's **own** gold symbol (`XAUUSD.c` on a
JustMarkets cent account, `XAUUSDm` on Exness...): the one the bridge traded on
that login before (`MQL5\Files\MuukBridge_<login>.symbol`), else the gold
symbol the broker server sent prices for (`bases\<server>\ticks`), else a gold
chart MT5 already has. A chart of a gold symbol the account can't trade (a
plain `XAUUSD` on a cent account) never loads -- MT5's journal says
"initializing of MuukBridge failed ... (symbol synchronization timeout /
execution canceled)" -- and the bridge never starts. `chartSymbol` in the
config can still name one by hand.

The account must be **hedging** (a netting account would close a position
instead of locking it) -- the bridge refuses netting accounts.

## What the bridge does inside MT5

- Only the account's gold symbol (XAUUSD, XAUUSDc, GOLD, ... found by
  itself) and only orders/positions with its magic number 770077: manual
  trades and other EAs are never touched.
- Pending Buy/Sell Stops for the range, market orders for locks and closes
  (a close is aimed at its own position ticket), cancels.
- Every fill, prices (polled every 25 ms), the account every second, the
  per-position swap. The bot's order id is kept in each order's comment, so
  a restarted MT5 still knows its orders.

## Any broker: taken from MT5 on connect

- price digits / step (2 or 3 digits), lot min / step / max, the broker's
  pending-order limit (max orders kept 2 below it);
- a first lot below the broker's minimum is raised to it (RoboForex ProCent
  gold: 0.10);
- money: P/L, commission and margin are counted in the account's currency --
  **US cents on a cent account** (shown as `12.50¢`), whatever the broker's
  lot convention:

| Broker | 1 lot | $1 move on 0.01 lot | Muuk counts |
|---|---|---|---|
| JustMarkets Standard Cent | a cent lot (1/100 of standard) | 1 USC | 100 per 1.00 per lot |
| RoboForex ProCent | 100 oz, min 0.10 | 100 USC | 10,000 per 1.00 per lot |

- margin: MT5's own margin for 1 lot at the current price and the symbol's
  hedged-margin rate (the Settings balance / leverage are only for cTrader).

Cent accounts are usually commission-free (`commissionPerMillionPerSide` 0)
with a wider spread -- the auto target follows the real spread.

## Speed

Stop orders sit at the broker, so their speed is the broker's. The bridge
adds up to one 25 ms poll for fills and market orders -- small next to a $2
range. Keep MT5 and Muuk on the same PC/VPS.

## Tested

The bridge and the whole bot are tested with a simulated MT5 over the same
pipe (`engine/mt5_bridge/tests`); the bridge compiles cleanly in MetaEditor.
First run on a **demo** account: open a range, let a level fill, press
Close range, and check that MT5's positions and history match Muuk's.
