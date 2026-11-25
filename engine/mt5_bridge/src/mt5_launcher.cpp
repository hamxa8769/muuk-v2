#include "muuk/mt5_bridge/mt5_launcher.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "muuk/ctrader_fix/instance.hpp"

namespace muuk::mt5_bridge {

namespace fs = std::filesystem;
namespace instance = muuk::ctrader_fix::instance;

namespace {

const unsigned char kEaSource[] = {
#include "muuk_bridge_ea.inc"
};

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

#ifdef _WIN32  // only the Win32 process matching below uses it
std::wstring normalized(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(p, ec);
    if (ec) abs = p;
    std::wstring s = lower(abs.wstring());
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}
#endif

// Platform-neutral key for deduping terminal paths (used by the merge,
// which is covered by tests on any system).
std::string pathKey(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(p, ec);
    if (ec) abs = p;
    std::string s = abs.generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// MT5's own text files (presets, startup files) are UTF-16LE.
bool writeUtf16(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.put(static_cast<char>(0xFF));
    out.put(static_cast<char>(0xFE));
    for (const char ch : text) {  // ASCII content only (names, numbers, the password as typed)
        out.put(ch);
        out.put('\0');
    }
    return static_cast<bool>(out);
}

bool isGoldName(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s.rfind("XAUUSD", 0) == 0 || s.rfind("GOLD", 0) == 0;
}

// A chart file's text (MT5 writes them in UTF-16LE, older ones in UTF-8/ANSI).
std::string chartText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::string out;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const unsigned c = static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
            out.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        return out;
    }
    return bytes;
}

std::string chartSymbolOf(const std::string& text) {
    const auto at = text.find("\nsymbol=");
    const std::size_t start = at == std::string::npos ? (text.rfind("symbol=", 0) == 0 ? 7 : std::string::npos) : at + 8;
    if (start == std::string::npos) return {};
    const auto end = text.find_first_of("\r\n", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\0')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\xEF' || s[i] == '\xBB' || s[i] == '\xBF')) ++i;  // UTF-8 BOM
    return s.substr(i);
}

std::string notOwnMt5Message(const fs::path& terminal) {
    return "Refused: " + terminal.string() + " isn't this copy's MetaTrader 5 (no " + instance::kOwnMt5Marker +
           " next to it). Production's MT5 must stay untouched: see docs/mt5.md, 'This copy's own MT5'.";
}

// A MuukBridge symbol file name: MuukBridge_<login>.symbol. The bridge EA
// (tools/mt5/MuukBridge.mq5) writes one into MQL5\Files for the account it
// runs on, so its presence says this folder has run the bridge before.
bool isBridgeSymbolFile(const std::string& name, std::string* loginOut) {
    const std::string prefix = "MuukBridge_";
    const std::string suffix = ".symbol";
    if (name.size() <= prefix.size() + suffix.size() || name.rfind(prefix, 0) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    if (loginOut) *loginOut = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    return true;
}

}  // namespace

bool isOwnMt5(const fs::path& terminal) {
    std::error_code ec;
    return fs::is_regular_file(terminal.parent_path() / instance::kOwnMt5Marker, ec);
}

std::string knownGoldSymbol(const fs::path& dataDir, const std::string& login, const std::string& server) {
    std::error_code ec;
    // 1. What the bridge traded on this account before (it writes this file once connected).
    if (!login.empty()) {
        const std::string sym = trimmed(readFile(dataDir / "MQL5" / "Files" / ("MuukBridge_" + login + ".symbol")));
        if (isGoldName(sym)) return sym;
    }
    // 2. The gold symbol this broker server has sent prices for (bases\<server>\ticks);
    //    a symbol the account can't trade has no ticks.
    if (server.empty()) return {};
    const std::wstring want = lower(fs::path(server).wstring());
    std::string best;
    fs::file_time_type newest{};
    for (const auto& srv : fs::directory_iterator(dataDir / "bases", ec)) {
        if (!srv.is_directory() || lower(srv.path().filename().wstring()) != want) continue;
        for (const auto& sym : fs::directory_iterator(srv.path() / "ticks", ec)) {
            if (!sym.is_directory() || !isGoldName(sym.path().filename().string())) continue;
            for (const auto& f : fs::directory_iterator(sym.path(), ec)) {
                const auto t = f.last_write_time(ec);
                if (!ec && (best.empty() || t > newest)) {
                    newest = t;
                    best = sym.path().filename().string();
                }
            }
        }
    }
    return best;
}

std::string bridgeEaSource() { return std::string(reinterpret_cast<const char*>(kEaSource), sizeof(kEaSource)); }

std::optional<fs::path> dataDirFor(const fs::path& terminal) {
    // Portable: the data lives in the install folder, once MT5 has run there.
    // Any MT5 counts here (broker or own): whether it may be closed or
    // restarted is a separate question, answered by isOwnMt5.
    std::error_code ec;
    const fs::path dir = terminal.parent_path();
    if (!fs::is_directory(dir / "MQL5", ec)) return std::nullopt;
    return dir;
}

std::vector<fs::path> findMt5DataDirs() {
    std::vector<fs::path> out;
#ifdef _WIN32
    wchar_t* v = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&v, &len, L"APPDATA") == 0 && v) {
        const fs::path root = fs::path(v) / "MetaQuotes" / "Terminal";
        free(v);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (e.is_directory()) out.push_back(e.path());
        }
    }
#endif
    return out;
}

std::optional<fs::path> chooseMt5DataDir(const std::optional<fs::path>& portable,
                                         const std::vector<fs::path>& candidates, const std::string& server,
                                         const std::string& login, std::string* why) {
    if (portable) return portable;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec)) dirs.push_back(c);
    }
    if (dirs.empty()) return std::nullopt;
    // The candidate that belongs to this broker account: its bases\ holds
    // this server (case insensitive), or it holds the bridge's symbol file
    // for this login.
    const std::wstring wantServer = lower(fs::path(server).wstring());
    for (const auto& d : dirs) {
        bool match = false;
        if (!server.empty()) {
            for (const auto& srv : fs::directory_iterator(d / "bases", ec)) {
                if (srv.is_directory() && lower(srv.path().filename().wstring()) == wantServer) {
                    match = true;
                    break;
                }
            }
        }
        if (!match && !login.empty() &&
            fs::is_regular_file(d / "MQL5" / "Files" / ("MuukBridge_" + login + ".symbol"), ec)) {
            match = true;
        }
        if (match) return d;
    }
    // No login/server match -- and the config need not have either (both are
    // optional): fall back to Muuk's own markers. The bridge EA writes
    // MQL5\Files\MuukBridge_<login>.symbol into the data folder it runs in,
    // so a folder that has run the bridge before is this account's folder
    // even when the config knows no login or server. If exactly one
    // candidate has one, take it; if several do, take the one whose symbol
    // file was written most recently (the folder the bridge ran in last).
    {
        std::error_code mec;
        std::optional<fs::path> best;
        fs::file_time_type bestTime{};
        std::string bestLogin;
        std::size_t bestCount = 0;
        for (const auto& d : dirs) {
            fs::file_time_type newest{};
            std::string newestLogin;
            std::size_t count = 0;
            for (const auto& e : fs::directory_iterator(d / "MQL5" / "Files", mec)) {
                std::string login;
                if (!e.is_regular_file(mec) || !isBridgeSymbolFile(e.path().filename().string(), &login)) continue;
                const auto t = e.last_write_time(mec);
                if (mec) continue;
                ++count;
                if (count == 1 || t > newest) {
                    newest = t;
                    newestLogin = login;
                }
            }
            if (count == 0) continue;
            // Newest symbol file wins; ties break by more symbol files, then
            // by path, so the choice is deterministic.
            if (!best || newest > bestTime ||
                (newest == bestTime && (count > bestCount || (count == bestCount && d.string() < best->string())))) {
                best = d;
                bestTime = newest;
                bestLogin = newestLogin;
                bestCount = count;
            }
        }
        if (best) {
            if (why) *why = "MuukBridge symbol file for account " + bestLogin + " found there";
            return best;
        }
        // Second marker: Muuk's own EA installed here
        // (MQL5\Experts\MuukUpgrade\MuukUpgradeBridge.ex5). Same rule: the
        // most recently installed wins.
        std::optional<fs::path> bestEa;
        fs::file_time_type bestEaTime{};
        for (const auto& d : dirs) {
            const fs::path ex5 =
                d / "MQL5" / "Experts" / instance::kEaFolder / (std::string(instance::kEaName) + ".ex5");
            if (!fs::is_regular_file(ex5, mec)) continue;
            const auto t = fs::last_write_time(ex5, mec);
            if (mec) continue;
            if (!bestEa || t > bestEaTime || (t == bestEaTime && d.string() < bestEa->string())) {
                bestEa = d;
                bestEaTime = t;
            }
        }
        if (bestEa) {
            if (why) *why = "MuukBridge EA installed there";
            return bestEa;
        }
    }
    if (dirs.size() == 1) return dirs.front();
    return std::nullopt;
}

std::optional<fs::path> dataDirForTerminal(const fs::path& terminal, const std::string& server,
                                           const std::string& login, std::string* why) {
    return chooseMt5DataDir(dataDirFor(terminal), findMt5DataDirs(), server, login, why);
}

std::vector<fs::path> runningMt5Terminals() {
#ifdef _WIN32
    std::vector<fs::path> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (lower(pe.szExeFile) != L"terminal64.exe") continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &size)) out.emplace_back(path);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
#else
    return {};
#endif
}

std::vector<Mt5Install> mergeMt5Installs(std::vector<Mt5Install> scanned,
                                         const std::vector<fs::path>& running) {
    std::vector<Mt5Install> out = std::move(scanned);
    for (const auto& terminal : running) {
        const std::string key = pathKey(terminal);
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const Mt5Install& i) { return pathKey(i.terminal) == key; });
        if (it != out.end()) {
            it->running = true;
            it->owned = it->owned || isOwnMt5(terminal);
            if (it->dataDir.empty()) {
                if (const auto d = dataDirFor(terminal)) it->dataDir = *d;
            }
            continue;
        }
        Mt5Install i;
        i.terminal = terminal;
        i.owned = isOwnMt5(terminal);
        i.running = true;
        i.label = terminal.parent_path().filename().string();
        if (i.label.empty()) i.label = terminal.string();
        if (const auto d = dataDirFor(terminal)) i.dataDir = *d;
        out.push_back(i);
    }
    return out;
}

std::vector<Mt5Install> findMt5Installs() {
    std::vector<Mt5Install> scanned;
    std::vector<fs::path> roots;
#ifdef _WIN32
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        wchar_t* v = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&v, &len, var) == 0 && v) {
            roots.emplace_back(v);
            free(v);
        }
    }
    // Also the top of the drive this program runs from (F:\MT5Upgrade).
    wchar_t self[MAX_PATH * 2];
    const DWORD selfLen = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (selfLen > 0) roots.push_back(fs::path(std::wstring(self, selfLen)).root_path());
#endif
    std::error_code ec;
    for (const auto& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const fs::path exe = entry.path() / "terminal64.exe";
            if (!fs::is_regular_file(exe, ec)) continue;
            Mt5Install i;
            i.terminal = exe;
            i.label = entry.path().filename().string();
            i.owned = isOwnMt5(exe);
            if (const auto d = dataDirFor(exe)) i.dataDir = *d;
            scanned.push_back(i);
        }
    }
    // Running terminals too: a broker MT5 in a custom or portable location
    // is found even though no scan root holds it.
    return mergeMt5Installs(std::move(scanned), runningMt5Terminals());
}

bool isMt5Running(const fs::path& terminal) {
#ifdef _WIN32
    const std::wstring want = normalized(terminal);
    for (const auto& p : runningMt5Terminals()) {
        if (normalized(p) == want) return true;
    }
    return false;
#else
    (void)terminal;
    return false;
#endif
}

Mt5StartResult startMt5WithBridge(const Mt5StartOptions& o, const std::function<void(const std::string&)>& log) {
    Mt5StartResult r;
    auto say = [&](const std::string& s) {
        if (log) log("[mt5] " + s);
    };
    fs::path terminal = o.terminal;
    std::error_code ec;
    if (terminal.empty()) {
        const auto installs = findMt5Installs();
        if (installs.empty()) {
            r.message = "No MetaTrader 5 found on this PC -- open or install your broker's MT5, then press Connect again.";
            return r;
        }
        terminal = installs.front().terminal;
    }
    if (!fs::is_regular_file(terminal, ec)) {
        r.message = "MetaTrader 5 not found at " + terminal.string() + " (Settings > Account > MT5 terminal).";
        return r;
    }
    const bool owned = isOwnMt5(terminal);
    std::string dataWhy;
    const auto data = dataDirForTerminal(terminal, o.server, o.login, &dataWhy);
    if (!data) {
        if (findMt5DataDirs().size() > 1) {
            // Picking the terminal in Settings does NOT fix this: a
            // non-portable broker MT5 has no portable folder, so the folder
            // is resolved from %APPDATA% either way. What makes it
            // resolvable is the bridge having run once: it writes its marker
            // file into the folder it runs in.
            r.message = "More than one MT5 data folder on this PC and Muuk can't tell which one is yours yet -- "
                        "open MT5 yourself once, attach MuukBridge to the gold chart, and press Connect again. "
                        "MuukBridge writes its marker file into that folder, so the next start finds it by itself.";
        } else {
            r.message = "MetaTrader 5 at " + terminal.parent_path().string() +
                        " has no data folder yet -- open it once (so it creates one), then Connect again.";
        }
        return r;
    }
    if (!dataWhy.empty()) say("using MT5 data folder " + data->string() + " (" + dataWhy + ")");
    // Portable (its data lives in its install folder) or not: only a portable
    // terminal is started/compiled with /portable, so a broker MT5 keeps
    // using its real data folder.
    const bool portable = (*data == terminal.parent_path());
#ifndef _WIN32
    (void)portable;  // only the Windows launch/compile commands use /portable
#endif

    // 1. The EA: the bridge built into Muuk, compiled by this MT5's own MetaEditor.
    //    Installing an EA into a broker MT5 is a standard EA install.
    const std::string eaName = instance::kEaName;
    const fs::path eaDir = *data / "MQL5" / "Experts" / instance::kEaFolder;
    fs::create_directories(eaDir, ec);
    const fs::path mq5 = eaDir / (eaName + ".mq5");
    const fs::path ex5 = eaDir / (eaName + ".ex5");
    const std::string source = bridgeEaSource();
    const bool sourceChanged = readFile(mq5) != source;
    if (sourceChanged) {
        std::ofstream out(mq5, std::ios::binary | std::ios::trunc);
        out << source;
    }
    if (sourceChanged || !fs::is_regular_file(ex5, ec)) {
        const fs::path editor = terminal.parent_path() / "metaeditor64.exe";
        if (!fs::is_regular_file(editor, ec)) {
            r.message = "MetaEditor (metaeditor64.exe) is missing next to MT5 -- can't build " + eaName + ".";
            return r;
        }
        say("building " + eaName + " with " + editor.string());
#ifdef _WIN32
        fs::remove(ex5, ec);
        const fs::path logFile = eaDir / (eaName + ".compile.log");
        std::wstring cmd = L"\"" + editor.wstring() + L"\"" + (portable ? L" /portable" : L"") + L" /compile:\"" +
                           mq5.wstring() + L"\" /log:\"" + logFile.wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#endif
        if (!fs::is_regular_file(ex5, ec)) {
            r.message = eaName + " didn't compile -- see " + (eaDir / (eaName + ".compile.log")).string();
            return r;
        }
    }

    // 2. Its inputs: the pipe to this Muuk.
    const fs::path presets = *data / "MQL5" / "Presets";
    fs::create_directories(presets, ec);
    writeUtf16(presets / (eaName + ".set"), "PipeName=" + o.pipeName + "\r\nTradeSymbol=\r\nMagicNumber=770078\r\n"
                                            "TimerMs=25\r\nSlippagePoints=50\r\n");

    if (isMt5Running(terminal)) {
        r.alreadyRunning = true;
        if (owned) {
            r.message = "MetaTrader 5 is already open -- the bridge connects if " + eaName +
                        " is on a chart there (it stays on the chart once Muuk has started MT5 once). "
                        "If it doesn't connect, close MT5 and connect again.";
        } else {
            r.message = "MetaTrader 5 is already open -- drag " + eaName +
                        " from MT5's Navigator onto the gold (XAUUSD) chart once (it stays there afterwards); "
                        "the bridge connects as soon as it is on a chart. "
                        "Or press 'Restart MT5 with bridge' in Settings > Account.";
        }
        say(r.message);
        return r;
    }

    // 3. The chart: this account's gold symbol under its real name on this
    //    broker (XAUUSD.c, XAUUSDm...). A chart of a gold symbol the account
    //    can't trade never finishes loading ("symbol synchronization timeout")
    //    and the bridge never starts. Known from the bridge's last run on this
    //    account, else the gold symbol the server sent prices for, else a gold
    //    chart MT5 already has. Leftover bridge charts from earlier starts go
    //    (MT5 is closed now, so its profile files can be changed).
    std::string chartSymbol = o.chartSymbol;
    const bool autoChart = chartSymbol.empty() || chartSymbol == "XAUUSD" || chartSymbol == "auto";
    const std::string known = autoChart ? knownGoldSymbol(*data, o.login, o.server) : std::string{};
    fs::file_time_type newest{};
    std::string fromCharts;
    for (const auto& entry : fs::recursive_directory_iterator(*data / "MQL5" / "Profiles" / "Charts", ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".chr") continue;
        const std::string text = chartText(entry.path());
        const std::string sym = chartSymbolOf(text);
        if (autoChart && isGoldName(sym)) {
            const auto t = entry.last_write_time(ec);
            if (fromCharts.empty() || t > newest) {
                newest = t;
                fromCharts = sym;
            }
        }
        if (text.find(eaName) != std::string::npos) {
            fs::remove(entry.path(), ec);  // a bridge chart from an earlier start: the startup file adds a fresh one
        }
    }
    if (autoChart) chartSymbol = !known.empty() ? known : fromCharts;
    if (chartSymbol.empty() || chartSymbol == "auto") chartSymbol = "XAUUSD";
    say("bridge chart: " + chartSymbol);

    // 4. The startup file: log in, algo trading on, the bridge on a chart.
    std::ostringstream ini;
    if (!o.login.empty()) {
        ini << "[Common]\r\nLogin=" << o.login << "\r\n";
        if (!o.password.empty()) ini << "Password=" << o.password << "\r\n";
        if (!o.server.empty()) ini << "Server=" << o.server << "\r\n";
        ini << "KeepPrivate=1\r\nNewsEnable=0\r\n";
    }
    ini << "[Experts]\r\nAllowLiveTrading=1\r\nAllowDllImport=0\r\nEnabled=1\r\nAccount=0\r\nProfile=0\r\n";
    ini << "[StartUp]\r\nExpert=" << instance::kEaFolder << "\\" << eaName << "\r\nExpertParameters=" << eaName
        << ".set\r\nSymbol=" << chartSymbol << "\r\nPeriod=M1\r\n";
    const fs::path startup = *data / "muuk_startup.ini";
    if (!writeUtf16(startup, ini.str())) {
        r.message = "can't write " + startup.string();
        return r;
    }
#ifdef _WIN32
    std::wstring cmd = L"\"" + terminal.wstring() + L"\"" + (portable ? L" /portable" : L"") + L" /config:\"" +
                       startup.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, terminal.parent_path().wstring().c_str(),
                        &si, &pi)) {
        fs::remove(startup, ec);
        r.message = "couldn't start " + terminal.string() + " (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#endif
    // The file may hold the password: gone once MT5 has read it.
    std::thread([startup] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::error_code e;
        fs::remove(startup, e);
    }).detach();
    r.started = true;
    r.message = "MetaTrader 5 started (minimized) with " + eaName +
                (o.login.empty() ? std::string(" on its last account") : " for account " + o.login);
    say(r.message);
    return r;
}

Mt5StartResult restartMt5WithBridge(const Mt5StartOptions& o, const std::function<void(const std::string&)>& log,
                                    bool allowOtherTerminal) {
    fs::path terminal = o.terminal;
    if (terminal.empty()) {
        const auto installs = findMt5Installs();
        if (!installs.empty()) terminal = installs.front().terminal;
    }
    if (!terminal.empty() && !isOwnMt5(terminal) && !allowOtherTerminal) {  // before anything gets closed
        Mt5StartResult r;
        r.message = notOwnMt5Message(terminal);
        if (log) log("[mt5] " + r.message);
        return r;
    }
#ifdef _WIN32
    if (!terminal.empty() && isMt5Running(terminal)) {
        if (log) log("[mt5] closing MetaTrader 5 to start it again with the bridge");
        const std::wstring want = normalized(terminal);
        struct Ctx {
            std::wstring want;
        } ctx{want};
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL {
                const auto* c = reinterpret_cast<const Ctx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!h) return TRUE;
                wchar_t path[MAX_PATH * 2];
                DWORD size = static_cast<DWORD>(std::size(path));
                const bool match = QueryFullProcessImageNameW(h, 0, path, &size) && normalized(fs::path(path)) == c->want;
                CloseHandle(h);
                if (match) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        for (int i = 0; i < 100 && isMt5Running(terminal); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (isMt5Running(terminal)) {
            Mt5StartResult r;
            r.alreadyRunning = true;
            r.message = "MetaTrader 5 didn't close (a dialog open?) -- close it yourself, then press Connect.";
            if (log) log("[mt5] " + r.message);
            return r;
        }
    }
#endif
    Mt5StartOptions again = o;
    again.terminal = terminal;
    return startMt5WithBridge(again, log);
}

}  // namespace muuk::mt5_bridge
