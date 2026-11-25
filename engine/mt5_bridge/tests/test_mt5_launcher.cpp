// Pure MT5 launcher logic: choosing a data folder and merging the terminal
// list. Runs on Linux too (nothing here touches Windows or a real MT5).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "muuk/mt5_bridge/mt5_launcher.hpp"

namespace fs = std::filesystem;
using namespace muuk::mt5_bridge;

namespace {

fs::path freshDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& f) {
    fs::create_directories(f.parent_path());
    std::ofstream(f) << "x";
}

}  // namespace

TEST_CASE("chooseMt5DataDir: a portable folder wins over every %APPDATA% candidate") {
    const fs::path root = freshDir("muuk_choose_data_portable");
    const fs::path portable = root / "MT5Upgrade";
    fs::create_directories(portable / "MQL5");
    const fs::path appdata = root / "hash1";
    fs::create_directories(appdata / "bases" / "JustMarkets-Demo6");
    const auto got = chooseMt5DataDir(portable, {appdata}, "JustMarkets-Demo6", "1");
    REQUIRE(got);
    REQUIRE(*got == portable);
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: the candidate whose bases hold the server, in any case") {
    const fs::path root = freshDir("muuk_choose_data_server");
    const fs::path a = root / "aaa";
    const fs::path b = root / "bbb";
    fs::create_directories(a / "bases" / "OtherServer");
    fs::create_directories(b / "bases" / "justmarkets-demo6");  // server name in any case
    const auto got = chooseMt5DataDir(std::nullopt, {a, b}, "JustMarkets-Demo6", "");
    REQUIRE(got);
    REQUIRE(*got == b);
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: the candidate holding this login's bridge symbol file") {
    const fs::path root = freshDir("muuk_choose_data_login");
    const fs::path a = root / "aaa";
    const fs::path b = root / "bbb";
    fs::create_directories(a);
    touch(b / "MQL5" / "Files" / "MuukBridge_1300034695.symbol");
    const auto got = chooseMt5DataDir(std::nullopt, {a, b}, "", "1300034695");
    REQUIRE(got);
    REQUIRE(*got == b);
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: one candidate is used, several unmatched are not") {
    const fs::path root = freshDir("muuk_choose_data_single");
    const fs::path only = root / "only";
    fs::create_directories(only);
    REQUIRE(chooseMt5DataDir(std::nullopt, {only}, "", ""));
    const fs::path other = root / "other";
    fs::create_directories(other);
    REQUIRE_FALSE(chooseMt5DataDir(std::nullopt, {only, other}, "", ""));
    REQUIRE_FALSE(chooseMt5DataDir(std::nullopt, {}, "", ""));
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: a folder with a MuukBridge symbol file wins over unmatched ones") {
    const fs::path root = freshDir("muuk_choose_data_marker");
    const fs::path a = root / "aaa";
    const fs::path b = root / "bbb";
    const fs::path c = root / "ccc";
    fs::create_directories(a);
    touch(b / "MQL5" / "Files" / "MuukBridge_1300083764.symbol");
    fs::create_directories(c);
    std::string why;
    const auto got = chooseMt5DataDir(std::nullopt, {a, b, c}, "", "", &why);
    REQUIRE(got);
    REQUIRE(*got == b);
    REQUIRE(why.find("1300083764") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: among folders with symbol files, the newest wins") {
    const fs::path root = freshDir("muuk_choose_data_marker_newest");
    const fs::path a = root / "aaa";
    const fs::path b = root / "bbb";
    const fs::path fa = a / "MQL5" / "Files" / "MuukBridge_111.symbol";
    const fs::path fb = b / "MQL5" / "Files" / "MuukBridge_222.symbol";
    touch(fa);
    touch(fb);
    fs::last_write_time(fa, fs::file_time_type{} + std::chrono::hours(1));
    fs::last_write_time(fb, fs::file_time_type{} + std::chrono::hours(2));
    const auto got = chooseMt5DataDir(std::nullopt, {a, b}, "", "");
    REQUIRE(got);
    REQUIRE(*got == b);
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: with no symbol files, a folder with the installed EA wins") {
    const fs::path root = freshDir("muuk_choose_data_ea");
    const fs::path a = root / "aaa";
    const fs::path b = root / "bbb";
    fs::create_directories(a);
    touch(b / "MQL5" / "Experts" / "MuukUpgrade" / "MuukUpgradeBridge.ex5");
    std::string why;
    const auto got = chooseMt5DataDir(std::nullopt, {a, b}, "", "", &why);
    REQUIRE(got);
    REQUIRE(*got == b);
    REQUIRE(!why.empty());
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: with no symbol files, the newest installed EA wins") {
    const fs::path root = freshDir("muuk_choose_data_ea_newest");
    const fs::path a = root / "aaa";
    const fs::path b = root / "bbb";
    const fs::path ea = a / "MQL5" / "Experts" / "MuukUpgrade" / "MuukUpgradeBridge.ex5";
    const fs::path eb = b / "MQL5" / "Experts" / "MuukUpgrade" / "MuukUpgradeBridge.ex5";
    touch(ea);
    touch(eb);
    fs::last_write_time(ea, fs::file_time_type{} + std::chrono::hours(2));
    fs::last_write_time(eb, fs::file_time_type{} + std::chrono::hours(1));
    const auto got = chooseMt5DataDir(std::nullopt, {a, b}, "", "");
    REQUIRE(got);
    REQUIRE(*got == a);
    fs::remove_all(root);
}

TEST_CASE("chooseMt5DataDir: folders that don't exist are skipped") {
    const fs::path root = freshDir("muuk_choose_data_missing");
    const fs::path real = root / "real";
    fs::create_directories(real);
    REQUIRE(chooseMt5DataDir(std::nullopt, {root / "gone", real}, "", ""));
    fs::remove_all(root);
}

TEST_CASE("dataDirForTerminal: a portable folder is the data folder, marker or not") {
    const fs::path root = freshDir("muuk_data_dir_portable");
    const fs::path install = root / "BrokerMT5";
    fs::create_directories(install / "MQL5");
    touch(install / "terminal64.exe");
    REQUIRE(*dataDirForTerminal(install / "terminal64.exe", "Anything-Demo", "1") == install);
    REQUIRE(*dataDirFor(install / "terminal64.exe") == install);  // no marker needed
    const fs::path plain = root / "Plain";
    fs::create_directories(plain);
    touch(plain / "terminal64.exe");
    REQUIRE_FALSE(dataDirFor(plain / "terminal64.exe"));
    REQUIRE_FALSE(dataDirForTerminal(plain / "terminal64.exe", "", ""));
    fs::remove_all(root);
}

TEST_CASE("mergeMt5Installs: scanned and running terminals merge and dedupe by path") {
    const fs::path root = freshDir("muuk_merge_installs");
    const fs::path scannedDir = root / "BrokerA";
    const fs::path runningDir = root / "BrokerB";
    fs::create_directories(scannedDir / "MQL5");
    fs::create_directories(runningDir / "MQL5");
    touch(scannedDir / "terminal64.exe");
    touch(runningDir / "terminal64.exe");
    touch(scannedDir / "muuk_upgrade_mt5.marker");  // the scanned one is this copy's own

    Mt5Install scanned;
    scanned.terminal = scannedDir / "terminal64.exe";
    scanned.label = "BrokerA";
    scanned.owned = true;
    scanned.dataDir = scannedDir;
    const auto merged = mergeMt5Installs({scanned}, {scannedDir / "terminal64.exe", runningDir / "terminal64.exe"});
    REQUIRE(merged.size() == 2);
    REQUIRE(merged[0].terminal == scannedDir / "terminal64.exe");
    REQUIRE(merged[0].owned);
    REQUIRE(merged[0].running);  // the same path was also running: deduped, not doubled
    REQUIRE(merged[1].terminal == runningDir / "terminal64.exe");
    REQUIRE_FALSE(merged[1].owned);  // a broker MT5: no marker
    REQUIRE(merged[1].running);
    REQUIRE(merged[1].label == "BrokerB");
    REQUIRE(merged[1].dataDir == runningDir);
    fs::remove_all(root);
}

TEST_CASE("mergeMt5Installs: the same path in different cases is one terminal") {
    const fs::path root = freshDir("muuk_merge_case");
    const fs::path dir = root / "BrokerA";
    fs::create_directories(dir / "MQL5");
    touch(dir / "terminal64.exe");
    Mt5Install scanned;
    scanned.terminal = dir / "terminal64.exe";
    scanned.label = "BrokerA";
    const fs::path upper = root / "BROKERA" / "terminal64.exe";  // same path, different case
    const auto merged = mergeMt5Installs({scanned}, {upper});
    REQUIRE(merged.size() == 1);
    REQUIRE(merged[0].running);
    fs::remove_all(root);
}

TEST_CASE("mergeMt5Installs: nothing scanned and nothing running is nothing") {
    REQUIRE(mergeMt5Installs({}, {}).empty());
}

TEST_CASE("Windows-only lookups are empty on this system") {
    REQUIRE(findMt5DataDirs().empty());
    REQUIRE(runningMt5Terminals().empty());
    REQUIRE(findMt5Installs().empty());
    REQUIRE_FALSE(isMt5Running(fs::temp_directory_path() / "terminal64.exe"));
}
