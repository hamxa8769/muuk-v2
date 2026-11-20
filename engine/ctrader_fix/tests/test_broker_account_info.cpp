#include <chrono>
#include <exception>
#include <thread>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/fix_message.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"
#include "live_test_helpers.hpp"
#include "mock_fix_server.hpp"

using namespace muuk::core;
using namespace muuk::ctrader_fix;
using namespace muuk::ctrader_fix::test;
using namespace std::chrono_literals;

// An MT5 AccountInfo message as the bridge gateway builds it (see
// mt5_gateway.cpp sendAccountInfo): the figures in tags 20001..20026.
static FixMessage accountMessage(double balance, double equity, int seqNum) {
    FixMessage msg = serverHeaderMessage(msgtype::MuukAccountInfo, seqNum);
    msg.set(tag::MuukBalance, balance, 2);
    msg.set(tag::MuukEquity, equity, 2);
    msg.set(tag::MuukMargin, 0.0, 2);
    msg.set(tag::MuukFreeMargin, 0.0, 2);
    msg.set(tag::MuukCurrency, std::string("USC"));
    msg.set(tag::MuukLeverage, 100.0, 0);
    msg.set(tag::MuukVolumeMin, 0.01, 4);
    msg.set(tag::MuukVolumeStep, 0.01, 4);
    msg.set(tag::MuukLogin, std::string("1300083764"));
    msg.set(tag::MuukServer, std::string("JustMarkets-Demo6"));
    msg.set(tag::MuukCompany, std::string("Just Global Markets Ltd."));
    msg.set(tag::MuukHedging, std::string("Y"));
    msg.set(tag::MuukSymbol, std::string("XAUUSD"));
    msg.set(tag::MuukTickSize, 0.01, 8);
    msg.set(tag::MuukTickValue, 1.0, 8);
    msg.set(tag::MuukMarginPerLot, 100.0, 4);
    msg.set(tag::MuukDigits, int64_t{2});
    msg.set(tag::MuukVolumeMax, 100.0, 4);
    msg.set(tag::MuukLimitOrders, int64_t{200});
    msg.set(tag::MuukTradeMode, std::string("demo"));
    return msg;
}

TEST_CASE("parseBrokerAccountInfo treats a $0 account as known") {
    // The bug: balance 0 AND equity 0 used to read as "not heard from MT5
    // yet", so a $0 demo could never pass the account wait and the runner
    // looped on reconnect forever. Receiving the message IS the signal.
    const BrokerAccountInfo a = parseBrokerAccountInfo(accountMessage(0.0, 0.0, 1));
    REQUIRE(a.known);
    REQUIRE(a.balance == Catch::Approx(0.0));
    REQUIRE(a.equity == Catch::Approx(0.0));
    REQUIRE(a.login == "1300083764");
    REQUIRE(a.server == "JustMarkets-Demo6");
    REQUIRE(a.currency == "USC");
    REQUIRE(a.cent());
    REQUIRE_FALSE(a.real());
    REQUIRE(a.tickValue == Catch::Approx(1.0));
}

TEST_CASE("parseBrokerAccountInfo keeps positive figures and the swap total") {
    FixMessage msg = accountMessage(10000.0, 10050.0, 1);
    msg.set(tag::MuukCostPositionId, std::string("P1"));
    msg.set(tag::MuukCostSwap, -2.5, 2);
    const BrokerAccountInfo a = parseBrokerAccountInfo(msg);
    REQUIRE(a.known);
    REQUIRE(a.balance == Catch::Approx(10000.0));
    REQUIRE(a.equity == Catch::Approx(10050.0));
    REQUIRE(a.swapTotal == Catch::Approx(-2.5));
}

TEST_CASE("LiveRunner holds all orders on a $0 MT5 account and resumes when funded") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.platform = "mt5";
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    // The empty demo account answers, balance and equity both $0 -- the
    // runner must connect on it, not loop on the account wait.
    server.sendToClient(kTradeConn, accountMessage(0.0, 0.0, 2));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // The first range would arm here on a funded account -- on the $0
    // account no order may be sent at all (waitForMessage throws on
    // timeout, which is the pass here).
    bool orderSentWhileEmpty = false;
    try {
        (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1500ms);
        orderSentWhileEmpty = true;
    } catch (const std::exception&) {
        // Expected: nothing sent while the account is empty.
    }
    REQUIRE_FALSE(orderSentWhileEmpty);
    REQUIRE(runnerException == nullptr);

    // Fund the account: the next 1-second account update reports equity,
    // and the hold must lift by itself -- no restart, no reconnect. A tick
    // asks for the first range again, and the boundary orders go out.
    server.sendToClient(kTradeConn, accountMessage(10000.0, 10000.0, 3));
    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string("4336.50"));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string("4336.52"));
    server.sendToClient(kQuoteConn, snapshot);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 5s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 5s);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(runnerException == nullptr);
}
