#include "updater.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h defines min/max as macros unless NOMINMAX is set; without
// this, std::min( at line 489 breaks under MSVC (error C2589). Every other
// windows.h user in this app (main.cpp, open_path.cpp, ...) defines it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#endif

namespace muuk::desktop::updater {

namespace {

// ---- pure helpers ----

bool isAllDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string lowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

// ---- version parsing / comparison ----

bool parseVersion(std::string_view text, Version& out) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    if (text.empty()) return false;
    int parts[3] = {0, 0, 0};
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t dot = text.find('.', at);
        const std::string_view piece = dot == std::string_view::npos ? text.substr(at) : text.substr(at, dot - at);
        if (!isAllDigits(piece)) return false;
        int value = 0;
        const auto res = std::from_chars(piece.data(), piece.data() + piece.size(), value);
        if (res.ec != std::errc() || res.ptr != piece.data() + piece.size() || value < 0) return false;
        parts[i] = value;
        if (dot == std::string_view::npos) {
            if (i != 2) return false;  // too few components
            at = text.size();
        } else {
            if (i == 2) return false;  // too many components
            at = dot + 1;
        }
    }
    out = Version{parts[0], parts[1], parts[2]};
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool isNewerRelease(std::string_view latestTag, std::string_view currentVersion) {
    Version latest{};
    Version current{};
    if (!parseVersion(latestTag, latest) || !parseVersion(currentVersion, current)) return false;
    // Release tags are v2.0.x while the app reports 0.2.x: the tag is
    // the app version with its first two components swapped (v2.0.3 IS
    // app 0.2.3). Convert the tag back before comparing, or every
    // release looks newer than the app that ships it (2.0.3 > 0.2.3
    // numerically) and the button never stops offering the same version.
    const Version latestAsApp{latest.minor, latest.major, latest.patch};
    return compareVersions(latestAsApp, current) > 0;
}

std::string zipAssetName(std::string_view tag) {
    return "Muuk-" + std::string(tag) + "-Windows.zip";
}

bool keepFileOnUpdate(std::string_view relativePath) {
    // The file name only: "my_account.local.json" and "sub/x.local.json"
    // are configs, "config.json" and "x.local.JSON" (case-insensitive) is
    // decided on the extension alone.
    const std::size_t slash = relativePath.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
    return !endsWith(lowerAscii(name), ".local.json");
}


// The batch the app launches and then quits for: waits until the app has
// exited, copies the new release over the app folder (never touching the
// user's *.local.json configs), restarts the app, and deletes itself --
// but only on success, so a failed update leaves the batch and its log
// behind for diagnosis.
//
// The batch runs with no console window, so it must never sleep with
// `timeout`: without a console that command exits immediately, and the
// wait loop then burns its whole budget in milliseconds and gives up
// before the app has even started quitting (the v0.2.1 bug: download
// fine, app closed, nothing copied, nothing restarted). The wait uses
// `ping`, which sleeps fine with no console. Every step is logged to
// update.log next to the batch.
std::string buildUpdateBatch(const std::string& appDir, const std::string& extractedDir) {
    std::string bat;
    bat += "@echo off\r\n";
    bat += "rem Muuk self-update: copies the extracted release over the app folder, then\r\n";
    bat += "rem restarts the app. The user's config files (*.local.json) are never overwritten.\r\n";
    bat += "rem This batch runs with no console window: never sleep with `timeout` here,\r\n";
    bat += "rem it exits immediately without a console. The wait below uses `ping`.\r\n";
    bat += "setlocal\r\n";
    bat += "set \"APP_DIR=" + appDir + "\"\r\n";
    bat += "set \"SRC_DIR=" + extractedDir + "\"\r\n";
    bat += "set \"LOG=%~dp0update.log\"\r\n";
    bat += "set \"EXE_NAME=muuk-upgrade-desktop.exe\"\r\n";
    bat += "set \"FAILED=0\"\r\n";
    bat += "echo Muuk update started %DATE% %TIME% > \"%LOG%\"\r\n";
    bat += "echo APP_DIR=%APP_DIR% >> \"%LOG%\"\r\n";
    bat += "echo SRC_DIR=%SRC_DIR% >> \"%LOG%\"\r\n";
    bat += "set \"WAIT_SECS=0\"\r\n";
    bat += ":wait_for_exit\r\n";
    bat += "tasklist /FI \"IMAGENAME eq %EXE_NAME%\" 2>nul | find /I \"%EXE_NAME%\" >nul\r\n";
    bat += "if errorlevel 1 goto do_copy\r\n";
    bat += "set /a WAIT_SECS+=1\r\n";
    bat += "echo Waiting for %EXE_NAME% to exit (%WAIT_SECS%s) >> \"%LOG%\"\r\n";
    bat += "if %WAIT_SECS% GEQ 120 goto give_up\r\n";
    bat += "ping -n 2 127.0.0.1 >nul\r\n";
    bat += "goto wait_for_exit\r\n";
    bat += ":do_copy\r\n";
    bat += "echo App has exited, copying the new release... >> \"%LOG%\"\r\n";
    bat += "robocopy \"%SRC_DIR%\" \"%APP_DIR%\" /E /XF *.local.json /R:2 /W:2 /NFL /NDL /NJH /NJS >> \"%LOG%\" 2>&1\r\n";
    bat += "set \"RC=%ERRORLEVEL%\"\r\n";
    bat += "echo robocopy exit code: %RC% >> \"%LOG%\"\r\n";
    bat += "rem robocopy exit codes 0-7 mean success (copied, or nothing to do); 8+ is a failure.\r\n";
    bat += "if %RC% GEQ 8 goto give_up\r\n";
    bat += "echo Restarting %EXE_NAME%... >> \"%LOG%\"\r\n";
    bat += "if not exist \"%APP_DIR%\\%EXE_NAME%\" goto no_restart\r\n";
    bat += "start \"\" \"%APP_DIR%\\%EXE_NAME%\"\r\n";
    bat += "echo start exit code: %ERRORLEVEL% >> \"%LOG%\"\r\n";
    bat += "goto done\r\n";
    bat += ":no_restart\r\n";
    bat += "echo %EXE_NAME% not found in %APP_DIR% -- not restarted >> \"%LOG%\"\r\n";
    bat += "goto done\r\n";
    bat += ":give_up\r\n";
    bat += "set \"FAILED=1\"\r\n";
    bat += "echo Update FAILED or gave up at %DATE% %TIME% -- the old app was NOT restarted. >> \"%LOG%\"\r\n";
    bat += "goto end\r\n";
    bat += ":done\r\n";
    bat += "echo Update finished %DATE% %TIME% >> \"%LOG%\"\r\n";
    bat += ":end\r\n";
    bat += "rem On success the batch deletes itself; on failure it stays next to\r\n";
    bat += "rem update.log so the failure can be reported.\r\n";
    bat += "if \"%FAILED%\"==\"1\" goto keep\r\n";
    bat += "(goto) 2>nul & del \"%~f0\"\r\n";
    bat += ":keep\r\n";
    return bat;
}

// ---- release JSON ----

std::optional<ReleaseInfo> parseLatestRelease(const std::string& jsonText) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText);
    } catch (...) {
        return std::nullopt;
    }
    if (!j.is_object() || !j.contains("tag_name") || !j.at("tag_name").is_string()) return std::nullopt;
    const std::string tag = j.at("tag_name").get<std::string>();
    if (!j.contains("assets") || !j.at("assets").is_array()) return std::nullopt;
    const std::string want = zipAssetName(tag);
    for (const auto& a : j.at("assets")) {
        if (!a.is_object() || !a.contains("name") || !a.at("name").is_string()) continue;
        if (a.at("name").get<std::string>() != want) continue;
        if (!a.contains("browser_download_url") || !a.at("browser_download_url").is_string()) return std::nullopt;
        if (!a.contains("size") || !a.at("size").is_number_unsigned()) return std::nullopt;
        ReleaseInfo info;
        info.tag = tag;
        info.downloadUrl = a.at("browser_download_url").get<std::string>();
        info.size = a.at("size").get<std::uint64_t>();
        return info;
    }
    return std::nullopt;
}

// ---- Updater: state machine ----

Updater::Updater(std::string currentVersion) : currentVersion_(std::move(currentVersion)) {}

Updater::~Updater() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

Updater::State Updater::state() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::string Updater::latestTag() const {
    std::scoped_lock lock(mutex_);
    return latest_ ? latest_->tag : std::string();
}

std::string Updater::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

double Updater::downloadProgress() const { return progress_.load(); }

void Updater::setState(State s) {
    std::scoped_lock lock(mutex_);
    state_ = s;
}

void Updater::fail(const std::string& why) {
    std::scoped_lock lock(mutex_);
    error_ = why;
    state_ = State::Failed;
}

// Spawning is UI-thread only (the dashboard calls it), so the only race
// to close is with the previous worker: workerDone_ is set as the
// worker's very last action, after its final mutex use, so joining after
// seeing it can never deadlock and the old worker can never overwrite the
// new one's state.
void Updater::startBackgroundCheck() {
    if (!workerDone_.load()) return;  // a worker is still running
    {
        std::scoped_lock lock(mutex_);
        if (state_ == State::Checking || state_ == State::Downloading || state_ == State::Available ||
            state_ == State::ReadyToInstall) {
            return;  // already checking, downloading, or an update is already known
        }
        error_.clear();
        state_ = State::Checking;
    }
    if (worker_.joinable()) worker_.join();
    workerDone_ = false;
    worker_ = std::thread([this] {
        checkWorker();
        {
            std::scoped_lock lock(mutex_);
            if (state_ == State::Checking) state_ = State::Idle;  // check finished, nothing newer
        }
        workerDone_ = true;
    });
}

void Updater::checkNow() {
    if (!workerDone_.load()) return;
    {
        std::scoped_lock lock(mutex_);
        if (state_ == State::Checking || state_ == State::Downloading) return;
        error_.clear();
        state_ = State::Checking;
    }
    if (worker_.joinable()) worker_.join();
    workerDone_ = false;
    worker_ = std::thread([this] {
        checkWorker();
        {
            std::scoped_lock lock(mutex_);
            if (state_ == State::Checking) state_ = State::Idle;
        }
        workerDone_ = true;
    });
}

void Updater::beginInstall() {
    if (!workerDone_.load()) return;
    {
        std::scoped_lock lock(mutex_);
        if (state_ != State::Available || !latest_) return;
        error_.clear();
        state_ = State::Downloading;
    }
    if (worker_.joinable()) worker_.join();
    progress_ = 0.0;
    workerDone_ = false;
    worker_ = std::thread([this] {
        installWorker();
        workerDone_ = true;
    });
}

#ifdef _WIN32

namespace {

// ---- Windows-only helpers ----

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string win32Error(const char* what) {
    return std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
}

// One HTTPS GET (follows redirects, so the download URL's hop to the
// object store works). onChunk gets each piece; returning false aborts.
// Never throws.
bool httpGet(const std::string& url, const std::function<bool(const char*, std::size_t)>& onChunk,
             std::string& errorOut) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const std::wstring wurl = utf8ToWide(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        errorOut = win32Error("could not parse the download URL");
        return false;
    }
    const HINTERNET session = WinHttpOpen(L"Muuk-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        errorOut = win32Error("could not open a web session");
        return false;
    }
    const HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        errorOut = win32Error("could not connect to GitHub");
        return false;
    }
    const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    const HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        errorOut = win32Error("could not start the request");
        return false;
    }
    const DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, const_cast<DWORD*>(&redirectPolicy), sizeof(redirectPolicy));
    WinHttpSetTimeouts(req, 10000, 10000, 10000, 30000);
    const bool sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0;
    bool ok = sent && WinHttpReceiveResponse(req, nullptr) != 0;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &statusSize, WINHTTP_NO_HEADER_INDEX) != 0 &&
             status == 200;
    }
    if (ok) {
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                ok = false;
                errorOut = win32Error("could not read the response");
                break;
            }
            if (avail == 0) break;
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), std::min<DWORD>(avail, static_cast<DWORD>(buf.size())), &read) ||
                read == 0) {
                ok = false;
                errorOut = win32Error("could not read the response");
                break;
            }
            if (!onChunk(buf.data(), read)) {
                ok = false;
                errorOut = "download cancelled";
                break;
            }
        }
    }
    if (!ok && errorOut.empty()) errorOut = "GitHub answered HTTP " + std::to_string(status);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

std::filesystem::path tempUpdateDir() {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    std::filesystem::path dir = n > 0 ? std::filesystem::path(tmp) : std::filesystem::temp_directory_path();
    return dir / L"MuukUpdate";
}

std::filesystem::path appDir() {
    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0) return {};
    return std::filesystem::path(exe).parent_path();
}

// Runs a command line, waits up to timeoutMs, returns the exit code.
// False on timeout, on failure to start, or when *stop is set while
// waiting (the child is killed then, so quitting mid-install never hangs).
bool runCommand(const std::wstring& cmdline, DWORD timeoutMs, DWORD& exitCode, const std::atomic<bool>* stop) {
    std::wstring mutableCmd = cmdline;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                       &pi)) {
        return false;
    }
    DWORD waited = WAIT_TIMEOUT;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (waited == WAIT_TIMEOUT) {
        waited = WaitForSingleObject(pi.hProcess, 500);
        if (waited == WAIT_TIMEOUT && ((stop && stop->load()) || GetTickCount64() >= deadline)) break;
    }
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    DWORD code = 1;
    const bool got = GetExitCodeProcess(pi.hProcess, &code) != 0;
    exitCode = code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return got;
}

// True when the raw bytes contain the name (a zip lists it in every file
// header and in the central directory).
bool fileContainsName(const std::filesystem::path& file, const std::string& name) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const std::size_t overlap = name.size() > 0 ? name.size() - 1 : 0;
    std::vector<char> buf(64 * 1024);
    std::string tail;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        const std::string chunk = tail + std::string(buf.data(), static_cast<std::size_t>(got));
        if (chunk.find(name) != std::string::npos) return true;
        tail = chunk.size() > overlap ? chunk.substr(chunk.size() - overlap) : chunk;
    }
    return false;
}


}  // namespace

void Updater::checkWorker() {
    std::string body;
    std::string error;
    const bool ok = httpGet(kReleasesLatestUrl,
                            [&](const char* data, std::size_t n) {
                                body.append(data, n);
                                return !stop_.load();
                            },
                            error);
    if (!ok) {
        fail("could not reach GitHub (" + error + ")");
        return;
    }
    const auto info = parseLatestRelease(body);
    if (!info) {
        fail("GitHub's answer could not be read (no Windows release found)");
        return;
    }
    if (!isNewerRelease(info->tag, currentVersion_)) return;  // up to date: stay quiet
    {
        std::scoped_lock lock(mutex_);
        latest_ = *info;
        state_ = State::Available;
    }
}

void Updater::installWorker() {
    std::optional<ReleaseInfo> release;
    {
        std::scoped_lock lock(mutex_);
        release = latest_;
    }
    if (!release) {
        fail("no release to install");
        return;
    }
    std::error_code ec;
    const std::filesystem::path dir = tempUpdateDir();
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        fail("could not create the update folder");
        return;
    }
    const std::filesystem::path zipPath = dir / zipAssetName(release->tag);
    {
        std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("could not write the update file");
            return;
        }
        std::uint64_t written = 0;
        std::string error;
        const bool ok = httpGet(release->downloadUrl,
                                [&](const char* data, std::size_t n) {
                                    if (stop_.load()) return false;
                                    out.write(data, static_cast<std::streamsize>(n));
                                    if (!out) return false;
                                    written += n;
                                    if (release->size > 0)
                                        progress_ = std::min(1.0, static_cast<double>(written) /
                                                                        static_cast<double>(release->size));
                                    return true;
                                },
                                error);
        out.close();
        if (!ok) {
            std::filesystem::remove(zipPath, ec);
            fail("the update download failed (" + error + ")");
            return;
        }
    }
    // Verify before anything is touched: exact size, a real zip, and the
    // new desktop exe inside it. A bad file is deleted, never installed.
    const std::uint64_t gotSize = std::filesystem::file_size(zipPath, ec);
    if (ec || gotSize != release->size) {
        std::filesystem::remove(zipPath, ec);
        fail("the downloaded update is incomplete (wrong size) -- deleted, nothing was changed");
        return;
    }
    std::ifstream head(zipPath, std::ios::binary);
    char magic[4]{};
    head.read(magic, sizeof(magic));
    if (!head || magic[0] != 'P' || magic[1] != 'K' || magic[2] != 3 || magic[3] != 4 ||
        !fileContainsName(zipPath, "muuk-upgrade-desktop.exe")) {
        std::filesystem::remove(zipPath, ec);
        fail("the downloaded update is not a valid Muuk release -- deleted, nothing was changed");
        return;
    }
    const std::filesystem::path extracted = dir / "extracted";
    std::filesystem::remove_all(extracted, ec);
    ec.clear();
    const std::string psCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive "
                              "-LiteralPath '" +
                              zipPath.string() + "' -DestinationPath '" + extracted.string() + "' -Force\"";
    DWORD exitCode = 1;
    if (!runCommand(utf8ToWide(psCmd), 120000, exitCode, &stop_) || exitCode != 0) {
        std::filesystem::remove_all(extracted, ec);
        fail("the update could not be unpacked");
        return;
    }
    std::error_code ec2;
    const std::filesystem::path newExe = extracted / "muuk-upgrade-desktop.exe";
    if (!std::filesystem::is_regular_file(newExe, ec2) || std::filesystem::file_size(newExe, ec2) == 0) {
        std::filesystem::remove_all(extracted, ec);
        fail("the update has no desktop app inside it -- nothing was changed");
        return;
    }
    const std::filesystem::path app = appDir();
    if (app.empty()) {
        fail("the app folder could not be found -- nothing was changed");
        return;
    }
    const std::filesystem::path batPath = dir / "update.bat";
    {
        std::ofstream bat(batPath, std::ios::binary | std::ios::trunc);
        if (!bat) {
            fail("the update script could not be written -- nothing was changed");
            return;
        }
        bat << buildUpdateBatch(app.string(), extracted.string());
    }
    // Launch through the shell: it invokes cmd.exe with the quoting handled
    // for us, and the batch runs with no window. (A hand-built cmd.exe /c
    // command line hung on the CI runner -- the batch never ran.)
    const HINSTANCE launched = ShellExecuteW(nullptr, L"open", batPath.c_str(), nullptr, nullptr, SW_HIDE);
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        fail("the update script could not be started -- nothing was changed");
        return;
    }
    setState(State::ReadyToInstall);  // the dashboard quits now; the batch copies and restarts us
}

#else  // not _WIN32: the updater cannot run here; the workers stay quiet

void Updater::checkWorker() {}
void Updater::installWorker() {}

#endif  // _WIN32

}  // namespace muuk::desktop::updater
