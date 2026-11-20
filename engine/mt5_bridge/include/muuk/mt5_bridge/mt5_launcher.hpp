#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace muuk::mt5_bridge {

// A MetaTrader 5 installed on this PC.
struct Mt5Install {
    std::filesystem::path terminal;  // ...\terminal64.exe
    std::filesystem::path dataDir;   // portable: the install folder; broker MT5: its data folder, once resolved
    std::string label;               // the install folder's name, e.g. "MT5Upgrade"
    bool owned = false;              // marker next to it: this copy's own MT5 (see isOwnMt5)
    bool running = false;            // a terminal64.exe with this exact path is alive right now
};

// True when this terminal64.exe is this copy's own MT5 (instance::kOwnMt5Marker
// next to it). Only an owned terminal is ever closed or restarted by Muuk
// on its own; anything else is the user's broker MT5.
[[nodiscard]] bool isOwnMt5(const std::filesystem::path& terminal);
// Every terminal64.exe found on this PC -- under Program Files and at the
// top of this program's drive (one folder deep), plus any terminal64.exe
// currently running (Toolhelp32 snapshot, so custom/portable locations are
// found too). A broker MT5 has no marker and is listed just the same, with
// owned=false; findMt5Installs never filters on the marker.
[[nodiscard]] std::vector<Mt5Install> findMt5Installs();
// The terminal64.exe files currently running, by exact path (Windows only;
// empty on other systems).
[[nodiscard]] std::vector<std::filesystem::path> runningMt5Terminals();
// Merges a scanned list with the running terminals: dedupes by normalized
// path, keeps owned from either side, marks the running ones. The pure
// merge/dedupe part is covered by tests on any system.
[[nodiscard]] std::vector<Mt5Install> mergeMt5Installs(std::vector<Mt5Install> scanned,
                                                       const std::vector<std::filesystem::path>& running);
// Every MT5 data folder under %APPDATA%\MetaQuotes\Terminal (Windows only;
// empty on other systems).
[[nodiscard]] std::vector<std::filesystem::path> findMt5DataDirs();
// The data folder of an MT5: its install folder, once it has run portable
// there (an MQL5 folder next to terminal64.exe -- no marker needed).
[[nodiscard]] std::optional<std::filesystem::path> dataDirFor(const std::filesystem::path& terminal);
// Picks the data folder for a terminal: its portable folder first, else the
// %APPDATA% candidate whose bases\ holds the configured server (case
// insensitive, the same match knownGoldSymbol uses) or whose MQL5\Files
// holds this login's bridge symbol file, else the only candidate there is,
// else nothing (the caller asks the user to pick). The pure choice is
// covered by tests on any system.
[[nodiscard]] std::optional<std::filesystem::path> chooseMt5DataDir(
    const std::optional<std::filesystem::path>& portable, const std::vector<std::filesystem::path>& candidates,
    const std::string& server, const std::string& login);
// The data folder for one terminal: portable first, else the %APPDATA%
// candidates matched by chooseMt5DataDir.
[[nodiscard]] std::optional<std::filesystem::path> dataDirForTerminal(const std::filesystem::path& terminal,
                                                                      const std::string& server,
                                                                      const std::string& login);
[[nodiscard]] bool isMt5Running(const std::filesystem::path& terminal);

struct Mt5StartOptions {
    std::filesystem::path terminal;  // empty = the first one found
    std::string login;               // empty = whatever account MT5 last used
    std::string password;            // only with login; written to a startup file that is deleted again
    std::string server;
    // The chart the bridge is attached to (it trades the gold symbol it finds). Empty or "XAUUSD" = the gold
    // symbol of a chart MT5 already has (brokers call it XAUUSD.c, XAUUSDc, GOLD...), else XAUUSD.
    std::string chartSymbol;
    std::string pipeName = "MuukUpgradeBridge_5300";
};

struct Mt5StartResult {
    bool started = false;         // MT5 was launched with the bridge
    bool alreadyRunning = false;  // MT5 was open already: the bridge connects if it's still on a chart
    std::string message;          // what happened, for the log / UI
};

// Gets MetaTrader 5 ready for Muuk: writes the MuukBridge EA (built into
// Muuk) into MT5's data folder and compiles it with MT5's own MetaEditor,
// writes its input preset (the pipe name), and starts MT5 minimized with a
// startup file that logs in (if a login is given), turns algo trading on and
// attaches MuukBridge to a chart. MT5 remembers that chart, so later starts
// run the bridge by themselves. This works for any MT5 on the PC, including
// the broker MT5 that is already open: installing the EA is a standard EA
// install. A terminal that is already running is never started again --
// instead the result says to drag MuukBridge onto the gold chart once (it
// stays there afterwards). A non-owned terminal is never started with
// /portable (its data lives in its real data folder, never redirected).
// Blocking (compiling takes a few seconds): call it off the UI thread.
[[nodiscard]] Mt5StartResult startMt5WithBridge(const Mt5StartOptions& options,
                                                const std::function<void(const std::string&)>& log);

// Closes a running MetaTrader 5 the way its own X button does (it saves its
// charts), waits for it to exit, then startMt5WithBridge. Never kills it: if
// it doesn't close within 20 s (e.g. a dialog is open), says so.
// A terminal that isn't this copy's own is refused unless
// allowOtherTerminal is set -- the UI sets that only from an explicit
// user-pressed "Restart MT5 with bridge" button, never from an automatic
// path (heal / account switch).
[[nodiscard]] Mt5StartResult restartMt5WithBridge(const Mt5StartOptions& options,
                                                  const std::function<void(const std::string&)>& log,
                                                  bool allowOtherTerminal = false);

// This account's gold symbol as MT5 already knows it, or empty: the one the
// bridge traded on this login before (MQL5\Files\MuukBridge_<login>.symbol),
// else the gold symbol this server last sent ticks for (bases\<server>\ticks).
[[nodiscard]] std::string knownGoldSymbol(const std::filesystem::path& dataDir, const std::string& login,
                                          const std::string& server);

// The MuukBridge.mq5 source built into this program.
[[nodiscard]] std::string bridgeEaSource();

}  // namespace muuk::mt5_bridge
