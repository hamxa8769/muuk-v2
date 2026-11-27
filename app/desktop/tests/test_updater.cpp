// Self-updater logic: version parsing/comparison, the GitHub release
// JSON, and the *.local.json keep rule. Runs on Linux too (nothing here
// touches Windows, WinHTTP or a real download).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "updater.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace muuk::desktop::updater;

TEST_CASE("parseVersion accepts v-prefixed and plain versions") {
    Version v{};
    REQUIRE(parseVersion("v2.0.1", v));
    REQUIRE(v.major == 2);
    REQUIRE(v.minor == 0);
    REQUIRE(v.patch == 1);
    REQUIRE(parseVersion("2.0.1", v));
    REQUIRE(parseVersion("0.2.1", v));
    REQUIRE(v.major == 0);
    REQUIRE(v.minor == 2);
    REQUIRE(v.patch == 1);
    REQUIRE(parseVersion("V10.20.30", v));
    REQUIRE(v.major == 10);
    REQUIRE(v.minor == 20);
    REQUIRE(v.patch == 30);
}

TEST_CASE("parseVersion rejects malformed input") {
    Version v{};
    REQUIRE_FALSE(parseVersion("", v));
    REQUIRE_FALSE(parseVersion("v", v));
    REQUIRE_FALSE(parseVersion("v2.0", v));
    REQUIRE_FALSE(parseVersion("2.0.1.4", v));
    REQUIRE_FALSE(parseVersion("abc", v));
    REQUIRE_FALSE(parseVersion("2.0.1-beta", v));
    REQUIRE_FALSE(parseVersion("2..1", v));
    REQUIRE_FALSE(parseVersion("2.0.x", v));
    REQUIRE_FALSE(parseVersion(" 2.0.1", v));
    REQUIRE_FALSE(parseVersion("2.0.1 ", v));
    REQUIRE_FALSE(parseVersion("-1.0.0", v));
}

TEST_CASE("compareVersions compares numerically, not as text") {
    Version a{2, 0, 9};
    Version b{2, 0, 10};
    REQUIRE(compareVersions(a, b) == -1);
    REQUIRE(compareVersions(b, a) == 1);
    REQUIRE(compareVersions(a, a) == 0);
    REQUIRE(compareVersions(Version{2, 0, 1}, Version{2, 1, 0}) == -1);
    REQUIRE(compareVersions(Version{3, 0, 0}, Version{2, 9, 9}) == 1);
}

TEST_CASE("isNewerRelease maps the release tag onto the app version") {
    // Tags are v2.x.y, the app reports 0.x.y: v2.4.0 IS app 0.4.0, so
    // the same release is not an update (v0.4.0 kept offering v2.4.0
    // because the old swap read the tag as app 4.2.0).
    REQUIRE_FALSE(isNewerRelease("v2.4.0", "0.4.0"));  // same release: no update
    REQUIRE(isNewerRelease("v2.4.1", "0.4.0"));        // the next release is newer
    REQUIRE(isNewerRelease("v2.5.0", "0.4.0"));        // the next minor is newer
    REQUIRE(isNewerRelease("v2.4.0", "0.3.9"));        // newer than the previous app
    REQUIRE_FALSE(isNewerRelease("v2.0.7", "0.4.0"));  // an old-scheme tag is older
    REQUIRE(isNewerRelease("v2.4.10", "0.4.9"));       // numeric, not text, comparison
    REQUIRE_FALSE(isNewerRelease("v2.0.1", "2.0.0"));  // an app at 2.0.0 is ahead of the v2.0.1 release
    REQUIRE_FALSE(isNewerRelease("v2.0", "0.2.1"));    // malformed tag
    REQUIRE_FALSE(isNewerRelease("v2.0.1", "banana"));  // malformed current
    REQUIRE_FALSE(isNewerRelease("", "0.2.1"));
}

TEST_CASE("zipAssetName names the Windows release asset") {
    REQUIRE(zipAssetName("v2.0.1") == "Muuk-v2.0.1-Windows.zip");
    REQUIRE(zipAssetName("v2.0.10") == "Muuk-v2.0.10-Windows.zip");
}

TEST_CASE("keepFileOnUpdate never overwrites the user's own configs") {
    REQUIRE_FALSE(keepFileOnUpdate("my_account.local.json"));
    REQUIRE_FALSE(keepFileOnUpdate("sub/dir/x.local.json"));
    REQUIRE_FALSE(keepFileOnUpdate("FILE.LOCAL.JSON"));  // case-insensitive
    REQUIRE_FALSE(keepFileOnUpdate("a\\b.local.json"));  // Windows separators too
    REQUIRE(keepFileOnUpdate("config.json"));
    REQUIRE(keepFileOnUpdate("mt5/mt5_account.example.json"));
    REQUIRE(keepFileOnUpdate("muuk-upgrade-desktop.exe"));
    REQUIRE(keepFileOnUpdate("local.json"));        // not a *.local.json file
    REQUIRE(keepFileOnUpdate("x.local.json.bak"));  // only the exact suffix counts
    REQUIRE_FALSE(keepFileOnUpdate("brain_v2.txt"));      // the user's live-learned brain is kept
    REQUIRE_FALSE(keepFileOnUpdate("sub/brain_v2.txt"));  // file name only, like the configs
    REQUIRE(keepFileOnUpdate("brain_v2.txt.bak"));        // only the exact name counts
}

namespace {
// A brain file whose header line 2 is exactly `header2` (line 1 is always
// "muuk-brain-v2 1").
std::filesystem::path writeBrainFile(const std::filesystem::path& dir, const std::string& name,
                                     const std::string& header2) {
    const std::filesystem::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "muuk-brain-v2 1\n" << header2 << "\n";
    return p;
}

std::filesystem::path freshBrainDir(const std::string& tag) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("muuk_brain_rule_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}
}  // namespace

// The v0.2.6 brain header, verbatim from the shipped file (backup at
// trainer/brain_v015_shipped_backup.txt): same trainedUntilMs as the new
// brain, no erMode field.
const char* kOldBrainHeader2 =
    "volLow 0.516437 volHigh 1.14914 trainedBars 1500000 trainedUntilMs 1793818980000 forced 0";
// The v0.2.7 brain header, verbatim from the repo-root brain_v2.txt.
const char* kNewBrainHeader2 =
    "volLow 0.516437 volHigh 1.14914 trainedBars 1500000 trainedUntilMs 1793818980000 forced 0 "
    "shiftAfterFills 3 stepAtr 2 erMode 2 stopLoss 0";

TEST_CASE("shouldInstallReleaseBrain installs the release brain over the v0.2.6 brain") {
    const auto root = freshBrainDir("old");
    const auto installed = writeBrainFile(root, "installed.txt", kOldBrainHeader2);
    const auto release = writeBrainFile(root, "release.txt", kNewBrainHeader2);
    // Same trainedUntilMs (both trained on the same 1.5M bars); the old
    // brain records no ER mode, so the release brain wins the tie.
    REQUIRE(shouldInstallReleaseBrain(installed, release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("shouldInstallReleaseBrain installs when the PC has no brain") {
    const auto root = freshBrainDir("missing");
    const auto release = writeBrainFile(root, "release.txt", kNewBrainHeader2);
    REQUIRE(shouldInstallReleaseBrain(root / "does-not-exist.txt", release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("shouldInstallReleaseBrain installs over an unparseable brain") {
    const auto root = freshBrainDir("garbage");
    const auto installed = root / "installed.txt";
    {
        std::ofstream out(installed, std::ios::binary | std::ios::trunc);
        out << "this is not a brain file\n";
    }
    const auto release = writeBrainFile(root, "release.txt", kNewBrainHeader2);
    REQUIRE(shouldInstallReleaseBrain(installed, release));
    // A file with the right magic but no trainedUntilMs is unparseable too.
    const auto noUntil = writeBrainFile(root, "no-until.txt", "volLow 0.5 volHigh 1.1 forced 0");
    REQUIRE(shouldInstallReleaseBrain(noUntil, release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("shouldInstallReleaseBrain installs over an older brain") {
    const auto root = freshBrainDir("older");
    const auto installed = writeBrainFile(
        root, "installed.txt",
        "volLow 0.516437 volHigh 1.14914 trainedBars 1500000 trainedUntilMs 1700000000000 forced 0 "
        "shiftAfterFills 3 stepAtr 2 erMode 2 stopLoss 0");
    const auto release = writeBrainFile(root, "release.txt", kNewBrainHeader2);
    REQUIRE(shouldInstallReleaseBrain(installed, release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("shouldInstallReleaseBrain keeps a newer locally-trained brain") {
    const auto root = freshBrainDir("newer");
    // The in-app trainer trained on bars past the release history: a
    // larger trainedUntilMs, with a recorded ER mode.
    const auto installed = writeBrainFile(
        root, "installed.txt",
        "volLow 0.516437 volHigh 1.14914 trainedBars 1600000 trainedUntilMs 1793905380000 forced 0 "
        "shiftAfterFills 3 stepAtr 2 erMode 2 stopLoss 0");
    const auto release = writeBrainFile(root, "release.txt", kNewBrainHeader2);
    REQUIRE_FALSE(shouldInstallReleaseBrain(installed, release));
    // Newer wins even without a recorded ER mode.
    const auto installedNoEr = writeBrainFile(
        root, "installed-no-er.txt",
        "volLow 0.516437 volHigh 1.14914 trainedBars 1600000 trainedUntilMs 1793905380000 forced 0");
    REQUIRE_FALSE(shouldInstallReleaseBrain(installedNoEr, release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("shouldInstallReleaseBrain keeps a same-age brain with a recorded ER mode") {
    const auto root = freshBrainDir("tie-recorded");
    // Same header as the release brain: local training wins the tie.
    const auto installed = writeBrainFile(root, "installed.txt", kNewBrainHeader2);
    const auto release = writeBrainFile(root, "release.txt", kNewBrainHeader2);
    REQUIRE_FALSE(shouldInstallReleaseBrain(installed, release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("shouldInstallReleaseBrain never installs an unparseable release brain") {
    const auto root = freshBrainDir("bad-release");
    const auto release = root / "release.txt";
    {
        std::ofstream out(release, std::ios::binary | std::ios::trunc);
        out << "garbage\n";
    }
    // Even with no usable brain on the PC, a release brain we cannot
    // validate is not installed.
    REQUIRE_FALSE(shouldInstallReleaseBrain(root / "does-not-exist.txt", release));
    const auto installed = writeBrainFile(root, "installed.txt", kOldBrainHeader2);
    REQUIRE_FALSE(shouldInstallReleaseBrain(installed, release));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("readBrainHeader reads only the header and tolerates CRLF") {
    const auto root = freshBrainDir("crlf");
    const auto p = root / "brain.txt";
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << "muuk-brain-v2 1\r\n" << kNewBrainHeader2 << "\r\n";
        out << "54 559 0.81 1 1 4 1.5 6 0.5 0\n";  // a cell line: never read
    }
    const auto header = readBrainHeader(p);
    REQUIRE(header.has_value());
    REQUIRE(header->valid);
    REQUIRE(header->trainedUntilMs == 1793818980000LL);
    REQUIRE(header->erMode == 2);
    const auto old = readBrainHeader(writeBrainFile(root, "old.txt", kOldBrainHeader2));
    REQUIRE(old.has_value());
    REQUIRE(old->erMode == -1);
    REQUIRE_FALSE(readBrainHeader(root / "does-not-exist.txt").has_value());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("buildUpdateBatch installs the release brain only when told to") {
    const std::string withBrain = buildUpdateBatch("C:\\Muuk", "C:\\Temp\\MuukUpdate\\extracted", true);
    // The unconditional copy is its own line (starts right after a CRLF).
    REQUIRE(withBrain.find("\r\ncopy /Y \"%SRC_DIR%\\brain_v2.txt\"") != std::string::npos);
    REQUIRE(withBrain.find("if not exist \"%APP_DIR%\\brain_v2.txt\" copy") == std::string::npos);
    // The bulk copy still never carries the brain blindly.
    REQUIRE(withBrain.find("/XF *.local.json brain_v2.txt") != std::string::npos);
    const std::string withoutBrain = buildUpdateBatch("C:\\Muuk", "C:\\Temp\\MuukUpdate\\extracted", false);
    REQUIRE(withoutBrain.find("if not exist \"%APP_DIR%\\brain_v2.txt\" copy /Y \"%SRC_DIR%\\brain_v2.txt\"") !=
            std::string::npos);
    REQUIRE(withoutBrain.find("\r\ncopy /Y \"%SRC_DIR%\\brain_v2.txt\"") == std::string::npos);
    // The default (two arguments) is the old copy-if-missing behaviour.
    const std::string def = buildUpdateBatch("C:\\Muuk", "C:\\Temp\\MuukUpdate\\extracted");
    REQUIRE(def.find("if not exist \"%APP_DIR%\\brain_v2.txt\" copy") != std::string::npos);
    REQUIRE(def.find("\r\ncopy /Y \"%SRC_DIR%\\brain_v2.txt\"") == std::string::npos);
}

TEST_CASE("parseLatestRelease finds the Windows zip asset") {
    const std::string json = R"({
        "tag_name": "v2.0.1",
        "assets": [
            {"name": "Muuk-v2.0.1.zip", "browser_download_url": "https://example.com/source.zip", "size": 123},
            {"name": "Muuk-v2.0.1-Windows.zip", "browser_download_url": "https://github.com/hamxa8769/muuk-v2/releases/download/v2.0.1/Muuk-v2.0.1-Windows.zip", "size": 1879040}
        ]
    })";
    const auto info = parseLatestRelease(json);
    REQUIRE(info.has_value());
    REQUIRE(info->tag == "v2.0.1");
    REQUIRE(info->downloadUrl ==
            "https://github.com/hamxa8769/muuk-v2/releases/download/v2.0.1/Muuk-v2.0.1-Windows.zip");
    REQUIRE(info->size == 1879040);
}

TEST_CASE("parseLatestRelease ignores releases without the Windows zip") {
    REQUIRE_FALSE(parseLatestRelease(R"({"tag_name": "v2.0.1", "assets": []})").has_value());
    REQUIRE_FALSE(parseLatestRelease(R"({"tag_name": "v2.0.1", "assets": [{"name": "Muuk-v2.0.1.zip", "browser_download_url": "https://example.com/x.zip", "size": 1}]})").has_value());
    REQUIRE_FALSE(parseLatestRelease("not json at all").has_value());
    REQUIRE_FALSE(parseLatestRelease(R"({"assets": []})").has_value());
    REQUIRE_FALSE(parseLatestRelease(R"({"tag_name": "v2.0.1"})").has_value());
    // The zip asset without a download URL is unusable.
    REQUIRE_FALSE(parseLatestRelease(R"({"tag_name": "v2.0.1", "assets": [{"name": "Muuk-v2.0.1-Windows.zip", "size": 9}]})").has_value());
}

TEST_CASE("Updater background check settles harmlessly") {
    Updater updater("0.2.1");
    REQUIRE(updater.state() == Updater::State::Idle);
    updater.startBackgroundCheck();
    // The worker finishes on its own; give it a moment, then it must
    // have settled -- never stuck in Checking. Off Windows there is no
    // network, so a clean check with nothing newer settles to UpToDate.
    // On Windows it really asks GitHub, and a failed check (no network,
    // or the unauthenticated API rate limit on a shared IP) legitimately
    // settles to Failed instead -- the check is best-effort by design.
    // Either way no release may be reported, since nothing newer was found.
    for (int i = 0; i < 2000 && updater.state() == Updater::State::Checking; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto settled = updater.state();
    REQUIRE((settled == Updater::State::UpToDate || settled == Updater::State::Failed));
    REQUIRE(updater.latestTag().empty());
    // beginInstall without an Available release is ignored, never a crash,
    // and leaves the settled state unchanged.
    updater.beginInstall();
    REQUIRE(updater.state() == settled);
}

TEST_CASE("a settled up-to-date check is not repeated, but a manual check re-runs") {
    Updater updater("0.2.1");
    updater.startBackgroundCheck();
    for (int i = 0; i < 2000 && updater.state() == Updater::State::Checking; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // On Windows an offline first check settles to Failed instead; the
    // no-repeat rule below only applies to a known up-to-date answer.
    if (updater.state() != Updater::State::UpToDate) {
        REQUIRE(updater.state() == Updater::State::Failed);
        return;
    }
    // The auto-check does not run twice in one run.
    updater.startBackgroundCheck();
    REQUIRE(updater.state() == Updater::State::UpToDate);
    // The manual button does re-check, and settles again (UpToDate here,
    // Failed on Windows when the network is gone).
    updater.checkNow();
    for (int i = 0; i < 2000 && updater.state() == Updater::State::Checking; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto resettled = updater.state();
    REQUIRE((resettled == Updater::State::UpToDate || resettled == Updater::State::Failed));
    REQUIRE(updater.latestTag().empty());
}

TEST_CASE("buildUpdateBatch never sleeps with timeout (v0.2.1 regression)") {
    const std::string bat = buildUpdateBatch("C:\\Muuk", "C:\\Temp\\MuukUpdate\\extracted");
    // The v0.2.1 batch slept with `timeout /t 1`; run with no console that
    // command exits immediately, so the wait for the app to quit burned its
    // whole budget in milliseconds and gave up before the app had even
    // started quitting. The new batch sleeps with ping instead.
    REQUIRE(bat.find("timeout /t") == std::string::npos);
    REQUIRE(bat.find("ping -n 2 127.0.0.1") != std::string::npos);
    // The wait is still bounded, and the app is still waited for first.
    REQUIRE(bat.find("tasklist") != std::string::npos);
    REQUIRE(bat.find("muuk-upgrade-desktop.exe") != std::string::npos);
    REQUIRE(bat.find("GEQ 120") != std::string::npos);
}

TEST_CASE("buildUpdateBatch logs, keeps itself on failure, and restarts the app") {
    const std::string bat =
        buildUpdateBatch("C:\\Users\\Asad Khan\\Muuk", "C:\\Temp\\MuukUpdate\\extracted");
    // Every step is logged next to the batch, so a failed update can be diagnosed.
    REQUIRE(bat.find("update.log") != std::string::npos);
    // The copy never overwrites the user's own configs.
    REQUIRE(bat.find("robocopy") != std::string::npos);
    REQUIRE(bat.find("/XF *.local.json") != std::string::npos);
    // The robocopy exit code is captured before anything can reset it
    // (an echo between robocopy and the check would reset ERRORLEVEL to 0).
    const std::size_t rcSet = bat.find("set \"RC=%ERRORLEVEL%\"");
    REQUIRE(rcSet != std::string::npos);
    REQUIRE(bat.find("if %RC% GEQ 8 goto give_up", rcSet) != std::string::npos);
    // Restart uses the empty-title form of start, so a path with spaces works.
    REQUIRE(bat.find("start \"\" \"%APP_DIR%\\%EXE_NAME%\"") != std::string::npos);
    // The batch deletes itself only on success; on failure it stays next
    // to update.log.
    REQUIRE(bat.find("if \"%FAILED%\"==\"1\" goto keep") != std::string::npos);
    REQUIRE(bat.find("del \"%~f0\"") != std::string::npos);
    // The injected folders are the ones passed in.
    REQUIRE(bat.find("C:\\Users\\Asad Khan\\Muuk") != std::string::npos);
    REQUIRE(bat.find("C:\\Temp\\MuukUpdate\\extracted") != std::string::npos);
}


#ifdef _WIN32

namespace {
// Runs a command line, waits up to timeoutMs, returns true when the child
// exited on its own. Mirrors the updater's own runCommand.
bool runForTest(const std::filesystem::path& batPath, DWORD timeoutMs) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = batPath.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return false;
    const DWORD waited = WaitForSingleObject(info.hProcess, timeoutMs);
    if (waited != WAIT_OBJECT_0) TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hProcess);
    return waited == WAIT_OBJECT_0;
}

std::string readFileText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFileText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// The v0.2.1 batch, exactly as it shipped: it sleeps with `timeout`,
// which exits immediately in a windowless batch, so the wait for the app
// to quit burns its whole budget in milliseconds and the update gives up
// before the app has even started quitting.
std::string oldBatchV021(const std::string& appDir, const std::string& srcDir) {
    std::string bat;
    bat += "@echo off\r\n";
    bat += "setlocal\r\n";
    bat += "set \"APP_DIR=" + appDir + "\"\r\n";
    bat += "set \"SRC_DIR=" + srcDir + "\"\r\n";
    bat += "set \"EXE_NAME=muuk-upgrade-desktop.exe\"\r\n";
    bat += "set \"WAIT_SECS=0\"\r\n";
    bat += ":wait_for_exit\r\n";
    bat += "tasklist /FI \"IMAGENAME eq %EXE_NAME%\" 2>nul | find /I \"%EXE_NAME%\" >nul\r\n";
    bat += "if errorlevel 1 goto do_copy\r\n";
    bat += "set /a WAIT_SECS+=1\r\n";
    bat += "if %WAIT_SECS% GEQ 60 goto give_up\r\n";
    bat += "timeout /t 1 /nobreak >nul\r\n";
    bat += "goto wait_for_exit\r\n";
    bat += ":do_copy\r\n";
    bat += "robocopy \"%SRC_DIR%\" \"%APP_DIR%\" /E /XF *.local.json /R:2 /W:2 /NFL /NDL /NJH /NJS\r\n";
    bat += "if errorlevel 8 goto give_up\r\n";
    bat += "start \"\" \"%APP_DIR%\\%EXE_NAME%\"\r\n";
    bat += ":give_up\r\n";
    bat += "(goto) 2>nul & del \"%~f0\"\r\n";
    return bat;
}
}  // namespace

// Runs the generated batch for real on Windows: a stand-in for the
// running desktop app (a copy of ping.exe renamed to
// muuk-upgrade-desktop.exe, kept alive by its own ping count) is started
// first, so the batch must wait for it to exit before copying. The
// fake app's lifetime is chosen per section: ~3s for the new batch,
// ~299s for the regression section.
TEST_CASE("Updater batch waits for the running app and copies the release (Windows)") {
    namespace fs = std::filesystem;
    // Catch2 runs this test case once per section, so each execution gets
    // its own temp dir: a leftover file (or a still-running fake app) from
    // one execution can never break the next one's setup.
    static int execution = 0;
    const fs::path root =
        fs::temp_directory_path() / ("muuk_updater_batch_test_" + std::to_string(execution++));
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path src = root / "src";
    const fs::path app = root / "app";
    fs::create_directories(src);
    fs::create_directories(app);
    writeFileText(src / "newfile.txt", "new");
    writeFileText(src / "my_account.local.json", "overwrite me");
    writeFileText(app / "my_account.local.json", "keep me");
    writeFileText(app / "old.txt", "old");

    wchar_t comspec[MAX_PATH]{};
    REQUIRE(GetEnvironmentVariableW(L"COMSPEC", comspec, static_cast<DWORD>(std::size(comspec))) > 0);
    // Both stand-ins are copies of ping.exe: it is a small system binary,
    // and run with "-n N 127.0.0.1" it lives (N - 1) seconds then exits.
    // (Copying cmd.exe instead was flaky on the runner -- run #13 failed
    // the copy with no further detail -- so ping.exe is used for both.)
    // Overwrite: the test case runs once per section (Catch2), and the
    // previous run cleanup can leave the file behind, so a stale
    // destination must not fail the copy.
    const fs::path pingExe = fs::path(comspec).parent_path() / "ping.exe";
    const auto copyWithRetry = [&](const fs::path& from, const fs::path& to) {
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            ec.clear();
            ok = fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return ok;
    };
    const fs::path fakeExe = root / "muuk-upgrade-desktop.exe";
    const bool copied = copyWithRetry(pingExe, fakeExe);
    if (!copied) std::cout << "copy of ping.exe to fake app failed: " << ec.message() << std::endl;
    REQUIRE(copied);
    REQUIRE(!ec);
    // The app folder gets a stand-in for the real app exe (a copy of
    // ping.exe: it exits at once with no arguments), so the batch's
    // restart step has a real program to launch instead of failing on a
    // missing file.
    const bool stubCopied = copyWithRetry(pingExe, app / "muuk-upgrade-desktop.exe");
    if (!stubCopied) std::cout << "copy of ping.exe to app dir failed: " << ec.message() << std::endl;
    REQUIRE(stubCopied);
    REQUIRE(!ec);

    const auto startFakeApp = [&](int pingCount) {
        // The fake app IS ping.exe (renamed): it lives (pingCount - 1)
        // seconds, then exits by itself.
        std::wstring cmd = L"\"" + fakeExe.wstring() + L"\" -n " + std::to_wstring(pingCount) + L" 127.0.0.1";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                               &pi) != 0);
        return pi;
    };
    const auto waitFakeApp = [&](PROCESS_INFORMATION& pi, DWORD timeoutMs = 15000) {
        const DWORD waited = WaitForSingleObject(pi.hProcess, timeoutMs);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return waited == WAIT_OBJECT_0;
    };

    SECTION("new batch waits, copies, logs, and deletes itself") {
        writeFileText(root / "update.bat", buildUpdateBatch(app.string(), src.string()));
        PROCESS_INFORMATION fake = startFakeApp(4);
        const bool ran = runForTest(root / "update.bat", 60000);
        if (!ran) {
            std::cout << "batch did not finish in 60s; log:\n"
                      << readFileText(root / "update.log") << "\nbatch still there: "
                      << fs::exists(root / "update.bat") << std::endl;
        }
        REQUIRE(waitFakeApp(fake));
        REQUIRE(ran);
        REQUIRE(readFileText(app / "newfile.txt") == "new");
        REQUIRE(readFileText(app / "my_account.local.json") == "keep me");
        REQUIRE(readFileText(app / "old.txt") == "old");
        const std::string log = readFileText(root / "update.log");
        const std::size_t codePos = log.find("robocopy exit code: ");
        REQUIRE(codePos != std::string::npos);
        const int rc = std::stoi(log.substr(codePos + std::string("robocopy exit code: ").size()));
        REQUIRE(rc < 8);  // robocopy 0-7 means success (copied, or nothing to do)
        REQUIRE(log.find("Update finished") != std::string::npos);
        REQUIRE(!fs::exists(root / "update.bat"));  // deleted itself on success
    }

    SECTION("v0.2.1 batch gives up while the app is still running (regression)") {
        writeFileText(root / "update.bat", oldBatchV021(app.string(), src.string()));
        // The app stays alive ~299s, far past the old batch's 60s wait
        // budget (even if each wait iteration is slow on the runner), so
        // the old batch must give up on its own (whether its timeout
        // ticks or fails instantly) instead of ever reaching the copy.
        PROCESS_INFORMATION fake = startFakeApp(300);
        const bool ran = runForTest(root / "update.bat", 180000);
        if (!ran) {
            std::cout << "batch did not finish in 180s; log:\n"
                      << readFileText(root / "update.log") << "\nbatch still there: "
                      << fs::exists(root / "update.bat") << std::endl;
        }
        REQUIRE(ran);
        REQUIRE(!fs::exists(app / "newfile.txt"));  // never copied: the old batch gave up
        REQUIRE(!fs::exists(root / "update.bat"));  // the old batch always deletes itself
        // The fake app outlives the test on purpose; stop it so it does
        // not linger and hold its exe file locked.
        TerminateProcess(fake.hProcess, 1);
        CloseHandle(fake.hThread);
        CloseHandle(fake.hProcess);
    }

    fs::remove_all(root, ec);
}

#endif  // _WIN32
