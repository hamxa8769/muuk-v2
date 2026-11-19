#pragma once

#include <string>

// What keeps this copy (the upgrade copy, F:\Muuk) apart from the production
// Muuk on the same PC: its own preferences folder, Windows names, EA, MT5 and
// local ports. Don't carry these values over when an upgrade goes to production.
namespace muuk::ctrader_fix::instance {

inline constexpr const char* kPrefsFolder = "MuukUpgrade";  // under %APPDATA%
inline constexpr const wchar_t* kWindowsName = L"Muuk Upgrade";  // HKCU Run value and <name>.lnk

// The EA as installed in MT5: MQL5\Experts\<folder>\<name>.mq5.
inline constexpr const char* kEaFolder = "MuukUpgrade";
inline constexpr const char* kEaName = "MuukUpgradeBridge";

// Only an MT5 with this file next to its terminal64.exe is ever installed
// into, started or closed; it runs with /portable, so its data stays in its
// own folder. Production's MT5 doesn't have this file.
inline constexpr const char* kOwnMt5Marker = "muuk_upgrade_mt5.marker";

inline constexpr int kDefaultEaPort = 5300;
inline constexpr int kDefaultFixPort = 5301;
// Production's: its bridge listens there, on the real account. Never used here.
inline constexpr int kProductionEaPort = 5200;
inline constexpr int kProductionFixPort = 5201;

inline std::string bridgePipeName(int eaPort) { return "MuukUpgradeBridge_" + std::to_string(eaPort); }

}  // namespace muuk::ctrader_fix::instance
