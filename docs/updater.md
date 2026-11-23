# Self-update (desktop app)

The desktop app updates itself from GitHub Releases — no manual zip
download, no reinstall. The version goes up with **every** update: each
change ships as a patch bump (0.2.0 → 0.2.1 → 0.2.2, …) with a new tag
(`v2.0.1`, …) and a new release. The running version is shown next to
`MUUK` in the header and in the window title (`… Range Bot v0.2.2`).

## How it works

1. **Check.** When the app starts it asks GitHub (background thread, never
   blocking the UI) for the latest release of the public repo
   `hamxa8769/muuk-v2` (`https://api.github.com/repos/hamxa8769/muuk-v2/releases/latest`).
   Offline? The check just stays silent — nothing is shown, nothing breaks.
   A small **Check for updates** button (header, next to Disconnect) checks
   again on demand.
2. **Button.** When a newer release exists, a green **Update available
   vX.Y.Z** button appears next to Connect/Disconnect. Nothing downloads
   or installs by itself.
3. **Download.** Pressing the button downloads `Muuk-<tag>-Windows.zip`
   to `%TEMP%\MuukUpdate\` (progress shown on the button). The file is
   verified before anything is touched: exact size from the release,
   a real zip, and `muuk-upgrade-desktop.exe` inside it. A bad download
   is deleted and a clear error is shown — the install is never touched.
4. **Install + restart.** The zip is unpacked to `%TEMP%\MuukUpdate\extracted`
   and a small `update.bat` is written and launched through `cmd.exe /c`. The
   app then quits cleanly (disconnects first, same as Disconnect). The batch
   waits until the app has exited (up to 120 s, sleeping with `ping` — the
   batch runs with no console window, and `timeout` does not work there: it
   exits immediately, which is why v0.2.1 closed the app but never copied
   or restarted anything), copies the new release over the app folder,
   restarts `muuk-upgrade-desktop.exe`, and deletes itself on success.
   Every step is logged to `%TEMP%\MuukUpdate\update.log`; on failure the
   batch stays next to the log instead of deleting itself, so the failure
   can be reported.

## What is kept

- **Your config files are never overwritten.** The copy step skips every
  `*.local.json` file (your own `my_account.local.json`, …). The zip ships
  a template `my_account.local.json`; yours stays exactly as it is.
- **Open positions stay at the broker.** The update does not close
  anything. After the restart the app reconnects and resumes the saved
  basket where it left off.
- **Nothing outside the app folder is touched.** The updater never touches
  the MT5 install, the bridge EA, or any other folder — only the app
  folder and `%TEMP%\MuukUpdate`.

## Safety

- The update source is fixed to `hamxa8769/muuk-v2`. There is no
  user-entered URL and no other source is ever consulted.
- The app never restarts by itself: the batch only runs after the
  operator presses **Update available**.
- If the app is still running after 120 s the batch gives up without
  copying anything — press the button again later.

## Troubleshooting

- **No button, no message:** the check found no newer release, or the PC
  is offline. Press **Check for updates** once the internet is back.
- **"The downloaded update is incomplete / not a valid Muuk release":**
  the download was bad and was deleted. Nothing changed; try again.
- **The app closed for the update but never reopened, or nothing changed:**
  open `%TEMP%\MuukUpdate\update.log` — it records every step (waiting,
  the robocopy exit code, the restart). If the update failed, `update.bat`
  is still next to the log; send the log when reporting it.
- **Leftovers:** `%TEMP%\MuukUpdate\` holds the last zip and extraction.
  Windows cleans `%TEMP%` itself; deleting the folder by hand is safe too
  (never while an update is running).

## For developers

The logic lives in `app/desktop/src/updater.{hpp,cpp}`. The version
parsing/comparison, the release-JSON parsing and the `*.local.json`
keep-rule are platform-independent and unit-tested on Linux
(`app/desktop/tests/test_updater.cpp`); the network/download/install
work is Windows-only (WinHTTP + PowerShell `Expand-Archive` + a
`robocopy` batch). The version comes from the single source of truth,
`project(VERSION ...)` in the top-level `CMakeLists.txt`, passed as
`MUUK_VERSION_STRING` (`app/desktop/src/version.hpp`).
