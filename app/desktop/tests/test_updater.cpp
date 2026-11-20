// Self-updater logic: version parsing/comparison, the GitHub release
// JSON, and the *.local.json keep rule. Runs on Linux too (nothing here
// touches Windows, WinHTTP or a real download).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "updater.hpp"

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

TEST_CASE("isNewerRelease spots a newer tag and ignores bad input") {
    REQUIRE(isNewerRelease("v2.0.1", "0.2.1"));
    REQUIRE(isNewerRelease("v2.0.1", "2.0.0"));
    REQUIRE(isNewerRelease("v2.0.10", "2.0.9"));
    REQUIRE_FALSE(isNewerRelease("v2.0.1", "2.0.1"));  // equal: no update
    REQUIRE_FALSE(isNewerRelease("v2.0.0", "2.0.1"));  // older
    REQUIRE_FALSE(isNewerRelease("v2.0", "2.0.1"));    // malformed tag
    REQUIRE_FALSE(isNewerRelease("v2.0.1", "banana"));  // malformed current
    REQUIRE_FALSE(isNewerRelease("", "2.0.1"));
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

TEST_CASE("Updater background check is a harmless no-op off Windows") {
    Updater updater("0.2.1");
    REQUIRE(updater.state() == Updater::State::Idle);
    updater.startBackgroundCheck();
    // The worker finishes on its own (no network off Windows); give it a
    // moment, then it must be back to Idle -- never stuck in Checking.
    for (int i = 0; i < 200 && updater.state() == Updater::State::Checking; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(updater.state() == Updater::State::Idle);
    REQUIRE(updater.latestTag().empty());
    // beginInstall without an Available release is ignored, never a crash.
    updater.beginInstall();
    REQUIRE(updater.state() == Updater::State::Idle);
}
