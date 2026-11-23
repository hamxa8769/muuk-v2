#pragma once

// Self-update for the desktop app: checks GitHub Releases for a newer Muuk,
// and (only after the operator presses the button) downloads the Windows
// zip, verifies it, installs it over the app folder and restarts.
//
// The update source is fixed: the public repo hamxa8769/muuk-v2, no auth,
// no user-entered URL. The check runs on a background thread and never
// blocks or crashes the app when offline -- a failed check just stays
// silent. The user's own config files (*.local.json) are never overwritten.
//
// The version parsing/comparison, the release-JSON parsing and the
// file-keep rule below are platform-independent and unit-tested on Linux
// (app/desktop/tests/test_updater.cpp). The network/download/install work
// is Windows-only (WinHTTP + PowerShell); on any other platform the
// Updater's workers are no-ops so the file still compiles and links.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace muuk::desktop::updater {

// ---- pure, platform-independent logic (unit-tested) ----

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

// "v2.0.1" or "2.0.1" -> {2, 0, 1}. Anything else ("v2.0", "abc", "",
// "2.0.1-beta", "2.0.1.4") is malformed and returns false.
[[nodiscard]] bool parseVersion(std::string_view text, Version& out);
// -1 when a < b, 0 when equal, 1 when a > b (numeric, so 2.0.10 > 2.0.9).
[[nodiscard]] int compareVersions(const Version& a, const Version& b);
// True when the release tag names a version newer than the running app.
// A malformed tag or current version means "no update", never an error.
[[nodiscard]] bool isNewerRelease(std::string_view latestTag, std::string_view currentVersion);
// The release asset for a tag: "Muuk-v2.0.1-Windows.zip".
[[nodiscard]] std::string zipAssetName(std::string_view tag);
// Builds the update.bat text for the given app folder and extracted
// release folder. Exposed for tests: the batch is plain text and is
// generated the same on every platform. It runs with no console window,
// so it must never sleep with `timeout` -- without a console that
// command exits immediately, and the wait for the app to quit gives up
// before the app has even started quitting (the v0.2.1 updater bug:
// the app closed, nothing was copied, nothing restarted). The wait
// uses `ping` instead, which sleeps fine without a console. The batch
// logs every step to update.log next to itself and deletes itself only
// on success, so a failure can be diagnosed from the log.
[[nodiscard]] std::string buildUpdateBatch(const std::string& appDir, const std::string& extractedDir);
// False for the user's own config files -- the updater must never copy
// over one. True for everything else in the zip. Case-insensitive, and
// the check is on the file name only (a folder named *.local.json is
// not a thing, but "sub/x.local.json" is still a config file).
[[nodiscard]] bool keepFileOnUpdate(std::string_view relativePath);

struct ReleaseInfo {
    std::string tag;          // e.g. "v2.0.1"
    std::string downloadUrl;  // browser_download_url of the Windows zip
    std::uint64_t size = 0;   // asset size in bytes, from the API
};

// Parses a GitHub /releases/latest response. nullopt when the JSON is
// malformed or holds no Windows zip asset for its tag.
[[nodiscard]] std::optional<ReleaseInfo> parseLatestRelease(const std::string& jsonText);

// ---- the updater itself ----

class Updater {
public:
    enum class State {
        Idle,          // up to date, or never checked (also: check failed quietly)
        Checking,      // asking GitHub for the latest release
        Available,     // a newer release exists -- the button is shown
        Downloading,   // the zip is coming down (progress() is 0..1)
        ReadyToInstall,// downloaded, verified, extracted, batch written and launched
        Failed,        // something went wrong; error() says what (shown, not alarming)
    };

    static constexpr const char* kRepo = "hamxa8769/muuk-v2";
    static constexpr const char* kReleasesLatestUrl =
        "https://api.github.com/repos/hamxa8769/muuk-v2/releases/latest";
    static constexpr const char* kUserAgent = "Muuk-Updater";

    explicit Updater(std::string currentVersion);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Starts the GitHub check on a background thread. Safe to call every
    // frame: a check already running (or a download in progress) is ignored.
    // Never throws, never blocks the UI thread.
    void startBackgroundCheck();
    // Manual re-check (the "Check for updates" button). Same as above,
    // but allowed again once the previous check finished.
    void checkNow();
    // Starts the download + install on a background thread. Only valid
    // from State::Available; anything else is ignored.
    void beginInstall();

    [[nodiscard]] State state() const;
    [[nodiscard]] std::string latestTag() const;  // e.g. "v2.0.1", "" when none
    [[nodiscard]] std::string error() const;      // why Failed, "" otherwise
    [[nodiscard]] double downloadProgress() const;  // 0..1 while Downloading
    [[nodiscard]] const std::string& currentVersion() const { return currentVersion_; }

private:
    void checkWorker();    // Windows: WinHTTP GET + parse; elsewhere: no-op
    void installWorker();  // Windows: download/verify/extract/batch/launch; elsewhere: no-op
    void setState(State s);
    void fail(const std::string& why);

    const std::string currentVersion_;
    mutable std::mutex mutex_;
    State state_ = State::Idle;
    std::optional<ReleaseInfo> latest_;
    std::string error_;
    std::atomic<double> progress_{0.0};
    std::thread worker_;
    std::atomic<bool> workerDone_{true};  // the worker sets this as its very last action
    std::atomic<bool> stop_{false};
};

}  // namespace muuk::desktop::updater
