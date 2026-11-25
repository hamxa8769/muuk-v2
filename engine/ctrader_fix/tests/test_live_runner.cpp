#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/core/market_brain_v2.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"
#include "live_test_helpers.hpp"
#include "mock_fix_server.hpp"

using namespace muuk::core;
using namespace muuk::ctrader_fix;
using namespace muuk::ctrader_fix::test;
using namespace std::chrono_literals;


TEST_CASE("LiveRunner resolves the symbol, arms both boundary orders, and re-arms on a fill") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    // run() blocks until stop() is called; exceptions are captured rather
    // than left to cross the thread boundary uncaught (which would call
    // std::terminate) and are surfaced via REQUIRE below, on the main
    // thread — Catch2's assertion macros are not safe to invoke off it.
    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    // LiveRunner connects and logs on to both sessions before doing
    // anything else (live_runner.cpp: tradeSession_->start() then
    // quoteSession_->start()), so both Logons must be acknowledged before
    // it will proceed to symbol resolution.
    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    // --- Symbol resolution ---
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    // Quote subscription happens before the engine (and so the initial
    // orders) is constructed -- see live_runner.cpp's run(): that
    // ordering is what lets auto-range-centering observe a live tick
    // first when enabled. autoRangeWidthPoints is 0 in this test's
    // config, so no tick is required before the engine starts; the
    // ordering still holds regardless.
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // --- The initial two boundary orders ---
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order1.getString(tag::Symbol) == "1");
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getInt(tag::OrdType) == 3);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    // OrderQty is lot hundredths on the wire (0.01 lot -> 1.0), not raw
    // lots -- see docs/ctrader-fix.md "Orders".
    REQUIRE(order1.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(order2.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    // --- A harmless in-range tick ---
    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string("4336.50"));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string("4336.52"));
    server.sendToClient(kQuoteConn, snapshot);

    // --- Broker fills the first (Buy Stop) order. Its opposite (order2,
    // Sell Stop) is already pending, so nothing new should be armed yet —
    // confirmed via snapshot() rather than waiting for a NewOrderSingle
    // that (correctly) never arrives. ---
    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);  // only order2 remains pending

    // --- The second (Sell Stop) order now fills. Its opposite (Buy) has
    // nothing pending, so a fresh Buy Stop (order3) is armed. ---
    FixMessage fill2 = serverHeaderMessage(msgtype::ExecutionReport, 4);
    fill2.set(tag::OrderID, std::string("9002"));
    fill2.set(tag::ClOrdID, order2ClOrdId);
    fill2.set(tag::PosMaintRptID, "P" + std::string(order2ClOrdId));
    fill2.set(tag::ExecType, std::string("F"));
    fill2.set(tag::OrdStatus, std::string("2"));
    fill2.set(tag::AvgPx, std::string("4336.00"));
    fill2.set(tag::LastQty, std::string("0.02"));
    fill2.set(tag::CumQty, std::string("0.02"));
    fill2.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill2);

    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order3.getInt(tag::Side) == 1);
    REQUIRE(order3.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    REQUIRE(order3.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(order3.getString(tag::ClOrdID) != order1ClOrdId);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner flattens the net position with a market order on basket close, "
          "and only starts the next cycle once that fill is confirmed") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 1.0;  // low bar, cleared by the rally below
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // --- Cycle 1's initial two boundary orders ---
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);

    // --- Buy Stop fills; only a long 0.01-lot leg is open. ---
    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);

    // --- Price rallies well past the profit target -> basket closes. The
    // engine cancels order2 (OrderCancelRequest, not a NewOrderSingle) and
    // emits CloseBasketCommand; LiveRunner must respond with an actual
    // Market order (OrdType=1) to flatten the 0.01-lot long, not just log
    // it and move on. ---
    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);  // Market, not Stop
    REQUIRE(closeOrder.getInt(tag::Side) == 2);      // net long -> flatten with a Sell
    // Aimed at that exact position -- without PosMaintRptID a hedging
    // account opens a new position instead of closing this one.
    REQUIRE(closeOrder.getString(tag::PosMaintRptID) == "P" + order1ClOrdId);
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    const std::string closeClOrdId = closeOrder.getString(tag::ClOrdID);

    // The basket is closed and the range is flat at the broker, but the
    // next cycle must NOT have started yet: no new boundary order should
    // exist until the closing trade itself is confirmed filled.
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Completed) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    // --- Now confirm the closing trade's fill. Only this unblocks cycle 2. ---
    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeClOrdId);
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4342.00"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    // Cycle 2 re-arms the same fixed range (no auto-centering here). The
    // market (4342) is already above its Buy Stop at 4337, which the broker
    // would refuse -- so that one waits for the price, and only the Sell
    // Stop goes out.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);  // Stop -- back to normal strategy entries
    REQUIRE(cycle2Order1.getInt(tag::Side) == 2);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4336.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);
    REQUIRE(runner.snapshot().activeOrderCount == 2);
    const auto cycle2 = runner.snapshot().pendingOrders;
    REQUIRE(cycle2.size() == 2);
    REQUIRE(std::count_if(cycle2.begin(), cycle2.end(), [](const PendingOrderInfo& o) { return o.waiting; }) == 1);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner closes every filled leg individually on basket close, "
          "not just a single net-sized order") {
    // Reproduces a hedging-account scenario: several fills whose *net*
    // exposure alone would not correctly describe what needs closing at
    // the broker (this demo account keeps every fill as its own position
    // rather than merging same-symbol exposure). Three legs end up open
    // (Buy .01, Sell .02, Buy .03) before the basket closes -- LiveRunner
    // must send three separate, individually-sized closing Market orders,
    // not one order for the net (long .02).
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 0.01;  // low bar: closes as soon as onTick sees any real profit
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell .02 @ 4336.00
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    auto sendFill = [&](int seqNum, const std::string& clOrdId, const std::string& px, const std::string& qty) {
        FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, seqNum);
        fill.set(tag::OrderID, std::to_string(9000 + seqNum));
        fill.set(tag::ClOrdID, clOrdId);
        fill.set(tag::PosMaintRptID, "P" + std::string(clOrdId));
        fill.set(tag::ExecType, std::string("F"));
        fill.set(tag::OrdStatus, std::string("2"));
        fill.set(tag::AvgPx, px);
        fill.set(tag::LastQty, qty);
        fill.set(tag::CumQty, qty);
        fill.set(tag::LeavesQty, std::string("0"));
        server.sendToClient(kTradeConn, fill);
    };

    // Leg 1: order1 (Buy .01) fills. Opposite (order2, Sell) already
    // pending, so nothing new is armed yet.
    sendFill(3, order1ClOrdId, "4337.00", "0.01");
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);

    // Leg 2: order2 (Sell .02) fills -> a fresh Buy Stop (order3) is armed.
    sendFill(4, order2ClOrdId, "4336.00", "0.02");
    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Buy .03 @ 4337.00
    const std::string order3ClOrdId = order3.getString(tag::ClOrdID);

    // Leg 3: order3 (Buy .03) fills -> a fresh Sell Stop (order4) is armed.
    sendFill(5, order3ClOrdId, "4337.00", "0.03");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Sell .04 @ 4336.00 -- will be cancelled

    // Three legs now open: Buy .01, Sell .02, Buy .03 (net long .02). A
    // rally clears the profit target -> basket closes, order4 is
    // cancelled, and all three legs must be flattened individually.
    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 6);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    // Closed in the order that keeps the open exposure smallest: net long
    // .02, so the Buy .03 first (leaves .01 short), then the Sell .02
    // (leaves .01 long), then the Buy .01.
    const FixMessage close1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close1.getInt(tag::OrdType) == 1);
    REQUIRE(close1.getInt(tag::Side) == 2);  // closes the Buy .03 leg
    REQUIRE(close1.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(close1.getString(tag::PosMaintRptID) == "P" + order3ClOrdId);

    const FixMessage close2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close2.getInt(tag::OrdType) == 1);
    REQUIRE(close2.getInt(tag::Side) == 1);  // closes the Sell .02 leg
    REQUIRE(close2.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(close2.getString(tag::PosMaintRptID) == "P" + order2ClOrdId);

    const FixMessage close3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close3.getInt(tag::OrdType) == 1);
    REQUIRE(close3.getInt(tag::Side) == 2);  // closes the Buy .01 leg
    REQUIRE(close3.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    REQUIRE(close3.getString(tag::PosMaintRptID) == "P" + order1ClOrdId);

    // Not one of these three closing orders has been confirmed filled yet
    // -- the next cycle must still be blocked.
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);

    sendFill(7, close1.getString(tag::ClOrdID), "4342.00", "0.03");
    sendFill(8, close2.getString(tag::ClOrdID), "4342.00", "0.02");
    // Two of three confirmed: still blocked.
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    sendFill(9, close3.getString(tag::ClOrdID), "4342.00", "0.01");

    // All three legs confirmed flat -> the next cycle starts.
    // The fixed range's Buy Stop (4337) is below the market (4342) here, so
    // it waits for the price; the Sell Stop is what goes out first.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4336.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner closes a cancelled order's fill immediately if it wins the race against its own cancel") {
    // Reproduces the reported "bot opened 2 positions when the range
    // started" bug: an order LiveRunner has already asked the broker to
    // cancel can still fill anyway (a real timing race -- the cancel
    // request and the fill can cross in flight). The surplus leg must be
    // closed right away, not silently left open until the next basket
    // close.
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 1.0;  // low bar, cleared by the rally below
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell .02 @ 4336.00
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }

    // Rally clears the profit target -> basket closes: order1's leg gets
    // an individual closing Market order, and order2 (still pending) gets
    // an OrderCancelRequest.
    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 4);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage closeOrder1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder1.getInt(tag::OrdType) == 1);
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);  // order2's cancel

    // The race: order2 fills anyway, despite the cancel already in flight.
    FixMessage fill2 = serverHeaderMessage(msgtype::ExecutionReport, 5);
    fill2.set(tag::OrderID, std::string("9002"));
    fill2.set(tag::ClOrdID, order2ClOrdId);
    fill2.set(tag::PosMaintRptID, "P" + std::string(order2ClOrdId));
    fill2.set(tag::ExecType, std::string("F"));
    fill2.set(tag::OrdStatus, std::string("2"));
    fill2.set(tag::AvgPx, std::string("4336.00"));
    fill2.set(tag::LastQty, std::string("0.02"));
    fill2.set(tag::CumQty, std::string("0.02"));
    fill2.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill2);

    // A second, separate closing Market order for the surplus leg must
    // appear -- not silence, not a wait for the next basket cycle.
    const FixMessage closeOrder2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder2.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder2.getInt(tag::Side) == 1);  // closes the Sell .02 leg
    REQUIRE(closeOrder2.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::closePosition and cancelOrder act on a single row without disturbing the rest") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const OrderId order1Id = static_cast<OrderId>(std::stoull(order1ClOrdId));
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell .02 @ 4336.00
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);
    const OrderId order2Id = static_cast<OrderId>(std::stoull(order2ClOrdId));

    // Fill order1 -> one open position, order2 stays a pending order.
    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }

    const LiveSnapshot beforeSnap = runner.snapshot();
    REQUIRE(beforeSnap.positions.size() == 1);
    REQUIRE(beforeSnap.positions[0].id == order1Id);
    REQUIRE(beforeSnap.positions[0].lots == Catch::Approx(0.01));
    REQUIRE(beforeSnap.pendingOrders.size() == 1);
    REQUIRE(beforeSnap.pendingOrders[0].id == order2Id);
    REQUIRE(beforeSnap.status == EngineStatus::Active);  // engine untouched by per-item actions

    runner.closePosition(order1Id);
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder.getInt(tag::Side) == 2);
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(1.0));

    runner.cancelOrder(order2Id);
    const FixMessage cancelReq = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancelReq.getString(tag::OrigClOrdID) == order2ClOrdId);

    for (int i = 0; i < 50; ++i) {
        const LiveSnapshot s = runner.snapshot();
        if (s.positions.empty() && s.pendingOrders.empty()) break;
        std::this_thread::sleep_for(50ms);
    }
    const LiveSnapshot afterSnap = runner.snapshot();
    REQUIRE(afterSnap.positions.empty());
    REQUIRE(afterSnap.pendingOrders.empty());
    // Per-item actions leave the engine running -- unlike closeAllPositions()/
    // cancelAllPendingOrders(), which reset it to Idle.
    REQUIRE(afterSnap.status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner snapshot shows every cost and a live net P/L; booked P/L comes from the real exit fill") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.costs.commissionPerLotRoundTurn = 7.0;  // $7 per lot round turn
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy Stop 0.01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell Stop, stays pending

    // Fills 0.10 worse than requested.
    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1ClOrdId);
    fill.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.10"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);

    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4337.99"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4338.01"));  // mid 4338.00
    server.sendToClient(kQuoteConn, tick);

    for (int i = 0; i < 50; ++i) {
        const LiveSnapshot s = runner.snapshot();
        if (s.positions.size() == 1 && s.bid.has_value()) break;
        std::this_thread::sleep_for(50ms);
    }
    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.positions.size() == 1);
    const PositionInfo& pos = snap.positions[0];
    REQUIRE(pos.requestedPrice == Catch::Approx(4337.00));
    REQUIRE(pos.entryPrice == Catch::Approx(4337.10));
    REQUIRE(pos.pnl.gross == Catch::Approx(1.00));       // 4337.00 -> 4338.00, 0.01 lot
    REQUIRE(pos.pnl.slippage == Catch::Approx(0.10));
    REQUIRE(pos.pnl.spread == Catch::Approx(0.01));      // mid -> bid
    REQUIRE(pos.pnl.commission == Catch::Approx(0.07));
    REQUIRE(pos.pnl.net == Catch::Approx(0.82));         // bid 4337.99 - fill 4337.10 = 0.89, minus 0.07
    REQUIRE(snap.live.net == Catch::Approx(0.82));
    REQUIRE(snap.live.costs() == Catch::Approx(0.18));

    // Close it; the broker fills the close at 4337.95 (a little worse than the bid).
    runner.closePosition(pos.id);
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4337.95"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    for (int i = 0; i < 50; ++i) {
        if (!runner.snapshot().closingInProgress) break;
        std::this_thread::sleep_for(50ms);
    }
    const LiveSnapshot after = runner.snapshot();
    REQUIRE(after.bookedGross == Catch::Approx(0.85));       // real fills: 4337.10 -> 4337.95
    REQUIRE(after.bookedCommission == Catch::Approx(0.07));
    REQUIRE(after.realizedPnl == Catch::Approx(0.78));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner records each closed basket with its position tree, and measures its own speed") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.costs.commissionPerLotRoundTurn = 7.0;
    LiveRunner runner(cfg);

    std::mutex savedMutex;
    std::vector<BasketRecord> saved;
    runner.setBasketHandler([&](const BasketRecord& b) {
        std::scoped_lock lock(savedMutex);
        saved.push_back(b);
    });

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy Stop 0.01 @ 4337.00
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);                      // Sell Stop, stays pending

    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1.getString(tag::ClOrdID));
    fill.set(tag::PosMaintRptID, std::string("P77"));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.00"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);

    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4338.00"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4338.02"));
    server.sendToClient(kQuoteConn, tick);
    for (int i = 0; i < 50; ++i) {
        const LiveSnapshot s = runner.snapshot();
        if (s.positions.size() == 1 && s.ticksProcessed > 0) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().currentBasket.has_value());
    REQUIRE(runner.snapshot().currentBasket->legs.size() == 1);

    runner.closeRange();
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::PosMaintRptID, std::string("P77"));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4338.00"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    for (int i = 0; i < 50; ++i) {
        {
            std::scoped_lock lock(savedMutex);
            if (!saved.empty()) break;
        }
        std::this_thread::sleep_for(50ms);
    }
    {
        std::scoped_lock lock(savedMutex);
        REQUIRE(saved.size() == 1);
        const BasketRecord& b = saved[0];
        REQUIRE(b.number == 1);
        REQUIRE(b.closeReason == "manual: close range");
        REQUIRE(b.legs.size() == 1);
        REQUIRE(b.legs[0].buy);
        REQUIRE(b.legs[0].positionId == "P77");
        REQUIRE(b.legs[0].entryPrice == Catch::Approx(4337.00));
        REQUIRE(b.legs[0].exitPrice == Catch::Approx(4338.00));
        REQUIRE(b.legs[0].gross == Catch::Approx(1.00));
        REQUIRE(b.legs[0].commission == Catch::Approx(0.07));
        REQUIRE(b.net == Catch::Approx(0.93));
        REQUIRE_FALSE(b.closedAt.empty());
    }

    const LiveSnapshot after = runner.snapshot();
    REQUIRE(after.baskets.size() == 1);
    REQUIRE_FALSE(after.currentBasket.has_value());
    REQUIRE(after.tickDecisionUs.samples >= 1);
    REQUIRE(after.brokerRoundTripMs.samples >= 1);  // order sent -> its fill report
    REQUIRE_FALSE(after.sessionStartedAt.empty());

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner never sends a close for a position whose id the broker didn't give it") {
    // On a hedging account an opposite order without PosMaintRptID opens a
    // *new* position -- so a leg with no known position id must not be
    // "closed" blindly.
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);

    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);  // deliberately no tag 721
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1.getString(tag::ClOrdID));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.00"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    const OrderId handle = runner.snapshot().positions.at(0).id;

    runner.closePosition(handle);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 400ms), std::runtime_error);
    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.positions.size() == 1);  // still listed -- it's still open at the broker
    REQUIRE(snap.closeFailed);            // and flagged, so no new range opens on top of it

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner syncs open positions from the broker on connect and can close them by position id") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.syncPositionsOnConnect = true;
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // Two XAUUSD positions left over from an earlier run, plus one on another symbol.
    const FixMessage request = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 3s);
    const std::string posReqId = request.getString(tag::PosReqID);
    auto report = [&](int seq, const char* positionId, const char* symbol, const char* longQty, const char* shortQty,
                      const char* price) {
        FixMessage r = serverHeaderMessage(msgtype::PositionReport, seq);
        r.set(tag::PosReqID, posReqId);
        r.set(tag::PosMaintRptID, std::string(positionId));
        r.set(tag::TotalNumPosReports, int64_t{3});
        r.set(tag::PosReqResult, std::string("0"));
        r.set(tag::Symbol, std::string(symbol));
        r.set(tag::NoPositions, int64_t{1});
        r.set(tag::LongQty, std::string(longQty));
        r.set(tag::ShortQty, std::string(shortQty));
        r.set(tag::SettlPrice, std::string(price));
        server.sendToClient(kTradeConn, r);
    };
    report(3, "P100", "1", "3", "0", "4280.00");  // BUY 0.03 lot
    report(4, "P200", "1", "0", "2", "4281.00");  // SELL 0.02 lot
    report(5, "P900", "2", "100000", "0", "1.10");  // EURUSD -- not ours, ignored
    // ...and no working orders.
    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 3s);
    FixMessage noOrders = serverHeaderMessage(msgtype::BusinessMessageReject, 6);
    noOrders.set(tag::RefMsgType, std::string("AF"));
    noOrders.set(tag::BusinessRejectRefID, ordReq.getString(tag::MassStatusReqID));
    server.sendToClient(kTradeConn, noOrders);
    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4280.00"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4280.10"));
    server.sendToClient(kQuoteConn, tick);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().connected) break;
        std::this_thread::sleep_for(50ms);
    }
    LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.positions.size() == 2);
    REQUIRE(snap.positions[0].adopted);
    REQUIRE(snap.positions[0].positionId == "P100");
    REQUIRE(snap.positions[0].side == OrderSide::BuyStop);
    REQUIRE(snap.positions[0].lots == Catch::Approx(0.03));
    REQUIRE(snap.positions[0].entryPrice == Catch::Approx(4280.00));
    REQUIRE(snap.positions[1].positionId == "P200");
    REQUIRE(snap.positions[1].side == OrderSide::SellStop);
    // autoContinue is on, but the account isn't flat -- no range opens.
    REQUIRE(snap.status == EngineStatus::Idle);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 400ms), std::runtime_error);

    runner.closeAllPositions();
    const FixMessage c1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    const FixMessage c2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(c1.getInt(tag::OrdType) == 1);
    REQUIRE(c1.getString(tag::PosMaintRptID) == "P100");
    REQUIRE(c1.getInt(tag::Side) == 2);
    REQUIRE(c1.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(c2.getString(tag::PosMaintRptID) == "P200");
    REQUIRE(c2.getInt(tag::Side) == 1);
    REQUIRE(c2.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::openRange/closeRange: one range at a time, never on top of an unfinished one") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoContinue = false;  // nothing opens until the operator asks
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().connected) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().connected);
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);  // autoContinue off: no orders yet

    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4300.00"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4300.10"));
    server.sendToClient(kQuoteConn, tick);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().bid.has_value()) break;
        std::this_thread::sleep_for(50ms);
    }

    // Tune before opening: $2 range, 0.02 starting lot.
    LiveSettings s = runner.settings();
    s.rangeWidth = 2.0;
    s.initialLot = 0.02;
    s.lotIncrement = 0.01;
    runner.applySettings(s);

    REQUIRE(runner.openRange());
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(buy.getInt(tag::Side) == 1);
    REQUIRE(buy.getDouble(tag::StopPx) == Catch::Approx(4301.05));  // mid 4300.05 + 1.00
    REQUIRE(buy.getDouble(tag::OrderQty) == Catch::Approx(2.0));     // 0.02 lot
    const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(sell.getInt(tag::Side) == 2);
    REQUIRE(sell.getDouble(tag::StopPx) == Catch::Approx(4299.05));
    REQUIRE(sell.getDouble(tag::OrderQty) == Catch::Approx(3.0));

    REQUIRE_FALSE(runner.openRange());  // one already running

    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, buy.getString(tag::ClOrdID));
    fill.set(tag::PosMaintRptID, "P" + std::string(buy.getString(tag::ClOrdID)));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4301.05"));
    fill.set(tag::LastQty, std::string("0.02"));
    fill.set(tag::CumQty, std::string("0.02"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().positions.size() == 1);

    // Close range: cancel the pending Sell, close the open Buy, go Idle.
    runner.closeRange();
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == sell.getString(tag::ClOrdID));
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder.getInt(tag::Side) == 2);
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.snapshot().closingInProgress);

    // The closing trade hasn't confirmed yet -- the account isn't flat, so
    // no new range may open.
    REQUIRE_FALSE(runner.openRange());

    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4300.00"));
    closeFill.set(tag::LastQty, std::string("0.02"));
    closeFill.set(tag::CumQty, std::string("0.02"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);
    for (int i = 0; i < 50; ++i) {
        if (!runner.snapshot().closingInProgress) break;
        std::this_thread::sleep_for(50ms);
    }

    // Flat again, and closeRange() left the engine Idle with auto-continue
    // off -- nothing opened by itself in the meantime.
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.openRange());
    const FixMessage reopened = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(reopened.getInt(tag::OrdType) == 3);
    // Order ids keep counting up across ranges -- the broker never sees a
    // ClOrdID reused.
    REQUIRE(std::stoull(reopened.getString(tag::ClOrdID)) > std::stoull(sell.getString(tag::ClOrdID)));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("saveLiveSettings rewrites only the tunable fields and round-trips through loadLiveConfig") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_settings.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD", "symbolId": "41"
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0
  }
})";
    }

    LiveSettings s;
    s.rangeWidth = 3.0;
    s.seedOffset = 0.10;
    s.initialLot = 0.02;
    s.lotIncrement = 0.02;
    s.profitTarget = 1.5;
    s.cooldownSeconds = 45;
    s.maxOrders = 12;
    s.maxLot = 1.0;
    s.maxBasketLoss = 200.0;
    s.autoContinue = false;
    s.startSide = StartSide::BuyOnly;
    s.commissionPerLot = 7.0;
    s.commissionPerMillion = 30.0;
    saveLiveSettings(path, s);

    const LiveConfig reloaded = loadLiveConfig(path);
    REQUIRE(reloaded.connection.password == "secret");  // credentials untouched
    REQUIRE(reloaded.connection.symbolId == "41");
    const LiveSettings back = settingsFromConfig(reloaded);
    REQUIRE(back.rangeWidth == Catch::Approx(3.0));
    REQUIRE(back.seedOffset == Catch::Approx(0.10));
    REQUIRE(back.initialLot == Catch::Approx(0.02));
    REQUIRE(back.lotIncrement == Catch::Approx(0.02));
    REQUIRE(back.profitTarget == Catch::Approx(1.5));
    REQUIRE(back.cooldownSeconds == 45);
    // Step 4 (D5, Section 4.2): maxOrders is capped by the knob's
    // maxLevels (Balanced: 6). Step 6b: the stop defaults to OFF, so it
    // reads back 0 -- the saved maxBasketLoss never migrates here (the
    // saved file has a risk section, so the no-risk-section migration does
    // not run) and the field itself reads back 0.
    REQUIRE(back.maxOrders == 6);
    REQUIRE(back.maxLot == Catch::Approx(1.0));
    REQUIRE(back.stopLoss == Catch::Approx(0.0));
    REQUIRE(back.maxBasketLoss == Catch::Approx(0.0));
    REQUIRE_FALSE(back.autoContinue);
    REQUIRE(back.startSide == StartSide::BuyOnly);
    REQUIRE(back.commissionPerLot == Catch::Approx(7.0));
    REQUIRE(back.commissionPerMillion == Catch::Approx(30.0));
    REQUIRE(reloaded.strategy.costs.commissionPerMillionPerSide == Catch::Approx(30.0));

    std::filesystem::remove(path);
}

TEST_CASE("API keys round-trip through saveLiveSettings and loadLiveConfig, and maskApiKey hides them") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_keys.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD", "symbolId": "41"
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0
  }
})";
    }

    LiveSettings s;
    s.keys.gemini = "AIzaSyTestGeminiKey1234";
    s.keys.fred = "fredkey5678";
    s.keys.finnhub = "finnhubkey90";
    saveLiveSettings(path, s);

    const LiveConfig reloaded = loadLiveConfig(path);
    REQUIRE(reloaded.keys.gemini == "AIzaSyTestGeminiKey1234");
    REQUIRE(reloaded.keys.fred == "fredkey5678");
    REQUIRE(reloaded.keys.finnhub == "finnhubkey90");
    REQUIRE(reloaded.connection.password == "secret");  // credentials untouched
    const LiveSettings back = settingsFromConfig(reloaded);
    REQUIRE(back.keys.gemini == "AIzaSyTestGeminiKey1234");
    // Masking: "Not set" when empty, four bullets + the last 4 characters otherwise.
    REQUIRE(maskApiKey("") == "Not set");
    REQUIRE(maskApiKey(back.keys.gemini) == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" "1234");
    REQUIRE(maskApiKey(back.keys.fred) == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" "5678");
    REQUIRE(maskApiKey("ab") == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" "ab");
    // A second save keeps the keys (the settings form round-trips them).
    saveLiveSettings(path, back);
    REQUIRE(loadLiveConfig(path).keys.finnhub == "finnhubkey90");

    std::filesystem::remove(path);
}

TEST_CASE("trainer history path round-trips through saveLiveSettings and loadLiveConfig") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_trainer_history.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD", "symbolId": "41"
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0
  }
})";
    }

    LiveSettings s;
    s.trainerHistoryPath = "C:\\Users\\test\\Documents\\MuukHistory";
    saveLiveSettings(path, s);

    const LiveConfig reloaded = loadLiveConfig(path);
    REQUIRE(reloaded.brain.trainerHistory == "C:\\Users\\test\\Documents\\MuukHistory");
    const LiveSettings back = settingsFromConfig(reloaded);
    REQUIRE(back.trainerHistoryPath == "C:\\Users\\test\\Documents\\MuukHistory");
    // Empty stays empty (the default: the bot's own bars).
    LiveSettings cleared;
    saveLiveSettings(path, cleared);
    REQUIRE(loadLiveConfig(path).brain.trainerHistory.empty());

    std::filesystem::remove(path);
}

TEST_CASE("Risk appetite presets map to the research doc's numbers") {
    // Section 4.4 / D11: Cautious {75, 4, 1%}, Balanced {150, 6, 2%},
    // Aggressive {300, 6, 3%}.
    const RiskPreset cautious = riskPresetFor(RiskAppetite::Cautious);
    REQUIRE(cautious.maxLockCost == Catch::Approx(75.0));
    REQUIRE(cautious.maxLevels == 4);
    REQUIRE(cautious.dailyLossPercent == Catch::Approx(1.0));
    const RiskPreset balanced = riskPresetFor(RiskAppetite::Balanced);
    REQUIRE(balanced.maxLockCost == Catch::Approx(150.0));
    REQUIRE(balanced.maxLevels == 6);
    REQUIRE(balanced.dailyLossPercent == Catch::Approx(2.0));
    const RiskPreset aggressive = riskPresetFor(RiskAppetite::Aggressive);
    REQUIRE(aggressive.maxLockCost == Catch::Approx(300.0));
    REQUIRE(aggressive.maxLevels == 6);
    REQUIRE(aggressive.dailyLossPercent == Catch::Approx(3.0));
    REQUIRE(std::string(riskAppetiteName(RiskAppetite::Cautious)) == "cautious");
    REQUIRE(std::string(riskAppetiteName(RiskAppetite::Balanced)) == "balanced");
    REQUIRE(std::string(riskAppetiteName(RiskAppetite::Aggressive)) == "aggressive");
    REQUIRE(riskAppetiteFromString("cautious") == RiskAppetite::Cautious);
    REQUIRE(riskAppetiteFromString("aggressive") == RiskAppetite::Aggressive);
    REQUIRE(riskAppetiteFromString("balanced") == RiskAppetite::Balanced);
    REQUIRE(riskAppetiteFromString("nope") == RiskAppetite::Balanced);  // unknown -> Balanced
}

TEST_CASE("effectiveDailyLossLimitMoney: the knob's share, else the $ amount, else off") {
    REQUIRE(effectiveDailyLossLimitMoney(2.0, 0.0, 500000.0) == Catch::Approx(10000.0));  // 2% of the balance
    REQUIRE(effectiveDailyLossLimitMoney(0.0, 500.0, 10000.0) == Catch::Approx(500.0));   // old $ amount kept
    REQUIRE(effectiveDailyLossLimitMoney(0.0, 0.0, 10000.0) == Catch::Approx(0.0));       // off
    REQUIRE(effectiveDailyLossLimitMoney(2.0, 500.0, 0.0) == Catch::Approx(500.0));       // balance unknown -> $
    REQUIRE(effectiveDailyLossLimitMoney(2.0, 0.0, 0.0) == Catch::Approx(0.0));           // neither known -> off
}

TEST_CASE("brainV2ConfigFor maps the knob and the ER mode onto the brain's config") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.risk.appetite = RiskAppetite::Cautious;
    cfg.brain.erMode = 2;
    const muuk::core::BrainV2Config cautious = brainV2ConfigFor(cfg);
    REQUIRE(cautious.erMode == 2);
    REQUIRE(cautious.caps.maxLockCost == Catch::Approx(75.0));
    REQUIRE(cautious.caps.maxLevelsHard == 4);
    cfg.risk.appetite = RiskAppetite::Balanced;
    cfg.brain.erMode = 0;
    const muuk::core::BrainV2Config balanced = brainV2ConfigFor(cfg);
    REQUIRE(balanced.erMode == 0);
    REQUIRE(balanced.caps.maxLockCost == Catch::Approx(150.0));  // the trainer's defaults exactly
    REQUIRE(balanced.caps.maxLevelsHard == 6);
    cfg.risk.appetite = RiskAppetite::Aggressive;
    const muuk::core::BrainV2Config aggressive = brainV2ConfigFor(cfg);
    REQUIRE(aggressive.caps.maxLockCost == Catch::Approx(300.0));
    REQUIRE(aggressive.caps.maxLevelsHard == 6);
    // Step 5: the panel's basket stop loss is mapped into replay units, so
    // the trainer validates the brain under the cap it will trade under.
    cfg.strategy.basketStopLossMoney = 50.0;
    REQUIRE(brainV2ConfigFor(cfg).stopLoss == Catch::Approx(50.0));  // test symbol: 1.0 per 0.01 tick -> scale 1.0
    cfg.strategy.basketStopLossMoney = 0.0;
    REQUIRE(brainV2ConfigFor(cfg).stopLoss == Catch::Approx(0.0));  // off stays off
    // A ProCent-style symbol (100 USC per 0.01 tick) counts 100 USC of P/L
    // as $1 replay: the $50 stop becomes 0.5 replay units.
    cfg.strategy.symbol.tickValue = 100.0;
    cfg.strategy.basketStopLossMoney = 50.0;
    REQUIRE(brainV2ConfigFor(cfg).stopLoss == Catch::Approx(0.5));
}

TEST_CASE("applySettingsToConfig clamps maxLot and maxOrders to the hard caps") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    LiveSettings s;
    s.maxLot = 10.0;     // an old file's no-limit-ish value
    s.maxOrders = 298;   // the user's old value
    s.riskAppetite = RiskAppetite::Balanced;
    applySettingsToConfig(cfg, s);
    REQUIRE(cfg.strategy.riskLimits.maxLot.toDouble(cfg.strategy.symbol) == Catch::Approx(1.0));
    REQUIRE(cfg.strategy.riskLimits.maxOrders == 6);
    s.riskAppetite = RiskAppetite::Cautious;
    applySettingsToConfig(cfg, s);
    REQUIRE(cfg.strategy.riskLimits.maxOrders == 4);
}

TEST_CASE("An old config file loads and migrates to the Step 4 defaults") {
    // Everything the old file had: the 7 removed options, no stop loss,
    // no daily limit, a 298 max orders, a 10.0 max lot, a $200 max basket
    // loss, and no "risk" section at all.
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_old_settings.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD", "symbolId": "41",
    "dailyLossLimitMoney": 0.0,
    "entry": {"rangeAtr": 2.0, "rangeMaxWidth": 15.0, "targetHalfRange": true, "trendMin": 1.0, "startWithTrend": true}
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0,
    "basketStopLossMoney": 0.0,
    "autoProfitTarget": {"enabled": true, "percent": 60},
    "riskLimits": {"maxOrders": 298, "maxLot": 10.0, "maxBasketLossMoney": 200.0}
  }
})";
    }
    const LiveConfig cfg = loadLiveConfig(path);  // must not throw
    REQUIRE(cfg.risk.appetite == RiskAppetite::Balanced);
    REQUIRE(cfg.risk.dailyLossPercent == Catch::Approx(2.0));  // the daily limit turns ON
    REQUIRE(cfg.strategy.basketStopLossMoney == Catch::Approx(200.0));  // D5: merged from max basket loss
    REQUIRE(cfg.strategy.riskLimits.maxBasketLossMoney == Catch::Approx(0.0));
    REQUIRE(cfg.strategy.riskLimits.maxOrders == 6);           // the knob's cap
    REQUIRE(cfg.strategy.riskLimits.maxLot.toDouble(cfg.strategy.symbol) == Catch::Approx(1.0));  // D9
    REQUIRE_FALSE(cfg.strategy.autoProfitTarget);               // D4: removed
    // The removed options are still read (old files load), just never used.
    REQUIRE(cfg.connection.entry.trendMin == Catch::Approx(1.0));
    REQUIRE(cfg.connection.entry.startWithTrend);
    REQUIRE(cfg.connection.entry.targetHalfRange);
    const LiveSettings back = settingsFromConfig(cfg);
    REQUIRE(back.stopLoss == Catch::Approx(200.0));
    REQUIRE(back.maxBasketLoss == Catch::Approx(0.0));
    REQUIRE_FALSE(back.autoTarget);
    REQUIRE(back.riskAppetite == RiskAppetite::Balanced);
    REQUIRE(back.dailyLossPercent == Catch::Approx(2.0));
    std::filesystem::remove(path);
}

TEST_CASE("An old config file with no stop and no max basket loss stays off (Step 6b)") {
    // Step 6b removed the $50 stop default: an old file that never set a
    // stop (and never set a max basket loss either) must load with the stop
    // OFF, not with $50 forced onto it.
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_old_no_stop.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD"
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0
  }
})";
    }
    const LiveConfig cfg = loadLiveConfig(path);
    REQUIRE(cfg.strategy.basketStopLossMoney == Catch::Approx(0.0));
    const LiveSettings back = settingsFromConfig(cfg);
    REQUIRE(back.stopLoss == Catch::Approx(0.0));
    std::filesystem::remove(path);
}

TEST_CASE("An old config file with a $ daily loss limit keeps it as the fallback") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_old_daily.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD", "dailyLossLimitMoney": 500.0
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0, "basketStopLossMoney": 50.0
  }
})";
    }
    const LiveConfig cfg = loadLiveConfig(path);
    REQUIRE(cfg.risk.dailyLossPercent == Catch::Approx(0.0));  // the $ amount stays the rule
    REQUIRE(cfg.connection.dailyLossLimitMoney == Catch::Approx(500.0));
    REQUIRE(effectiveDailyLossLimitMoney(cfg.risk.dailyLossPercent, cfg.connection.dailyLossLimitMoney, 10000.0) ==
            Catch::Approx(500.0));
    std::filesystem::remove(path);
}

TEST_CASE("The Step 4 risk settings round-trip through saveLiveSettings and loadLiveConfig") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_risk.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD"
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0
  }
})";
    }
    LiveSettings s;
    s.riskAppetite = RiskAppetite::Aggressive;
    s.dailyLossPercent = 3.0;
    s.erMode = 2;
    s.maxLot = 1.0;
    s.maxOrders = 6;
    s.stopLoss = 50.0;
    saveLiveSettings(path, s);
    const LiveConfig reloaded = loadLiveConfig(path);
    REQUIRE(reloaded.risk.appetite == RiskAppetite::Aggressive);
    REQUIRE(reloaded.risk.dailyLossPercent == Catch::Approx(3.0));
    REQUIRE(reloaded.brain.erMode == 2);
    REQUIRE(reloaded.strategy.riskLimits.maxLot.toDouble(reloaded.strategy.symbol) == Catch::Approx(1.0));
    REQUIRE(reloaded.strategy.riskLimits.maxOrders == 6);
    REQUIRE(reloaded.strategy.basketStopLossMoney == Catch::Approx(50.0));
    const LiveSettings back = settingsFromConfig(reloaded);
    REQUIRE(back.riskAppetite == RiskAppetite::Aggressive);
    REQUIRE(back.dailyLossPercent == Catch::Approx(3.0));
    REQUIRE(back.erMode == 2);
    REQUIRE(back.maxLot == Catch::Approx(1.0));
    REQUIRE(back.maxOrders == 6);
    REQUIRE(back.stopLoss == Catch::Approx(50.0));
    std::filesystem::remove(path);
}

TEST_CASE("LiveRunner::closeAllPositions closes every open leg and idles the engine") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // order2, left pending

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }

    // One open leg (Buy .01, from order1) and one still-pending order
    // (order2, Sell) at this point. closeAllPositions() must only touch
    // the open leg -- pending orders are cancelAllPendingOrders()'s job.
    runner.closeAllPositions();

    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);  // Market
    REQUIRE(closeOrder.getInt(tag::Side) == 2);      // opposite of the Buy leg
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(1.0));

    // The engine is now a fresh, Idle instance -- it will not act on its
    // own until the operator explicitly restarts the bot.
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Idle) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::flattenAndWait closes everything and returns only once the broker confirms it") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().positions.size() == 1);

    // The broker never confirms the close in time: the guard must say so
    // rather than claim the account is flat.
    REQUIRE_FALSE(runner.flattenAndWait(300ms));
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == order2.getString(tag::ClOrdID));
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder.getString(tag::PosMaintRptID) == "P" + order1ClOrdId);
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);

    // Called again while the close is still in flight: no second close is
    // sent, and it returns true as soon as the fill arrives.
    std::atomic<bool> finished{false};
    std::atomic<bool> flat{false};
    std::thread guard([&] {
        flat = runner.flattenAndWait(5s);
        finished = true;
    });
    std::this_thread::sleep_for(200ms);
    REQUIRE_FALSE(finished.load());

    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4336.90"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);
    guard.join();
    REQUIRE(flat.load());
    REQUIRE(runner.snapshot().positions.empty());
    REQUIRE(runner.snapshot().pendingOrders.empty());
    REQUIRE(runner.snapshot().baskets.size() == 1);
    REQUIRE(runner.snapshot().baskets.back().closeReason == "guard: app closing");

    REQUIRE(runner.flattenAndWait(1s));  // already flat: immediate

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::cancelAllPendingOrders cancels every pending order and idles the engine") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order1Id = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order2Id = order2.getString(tag::ClOrdID);

    runner.cancelAllPendingOrders();

    const FixMessage cancel1 = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage cancel2 = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const std::vector<std::string> cancelledIds = {cancel1.getString(tag::OrigClOrdID),
                                                    cancel2.getString(tag::OrigClOrdID)};
    REQUIRE(std::find(cancelledIds.begin(), cancelledIds.end(), order1Id) != cancelledIds.end());
    REQUIRE(std::find(cancelledIds.begin(), cancelledIds.end(), order2Id) != cancelledIds.end());

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Idle) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner waits out connection.basketCooldownSeconds after a basket "
          "is fully flat before starting the next cycle") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 1.0;
    cfg.connection.basketCooldownSeconds = 1;  // seconds -- short so the test stays fast
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // order2

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }

    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);

    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4342.00"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    // The basket is fully flat now (the only leg's closing trade just
    // confirmed), but the cooldown hasn't elapsed yet -- no new order
    // should appear within a window well short of the 1s cooldown.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 300ms), std::runtime_error);
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);

    // A quote tick arriving mid-cooldown must not bypass it either.
    FixMessage midCooldownTick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 5);
    midCooldownTick.set(tag::Symbol, std::string("1"));
    midCooldownTick.set(tag::NoMDEntries, int64_t{2});
    midCooldownTick.set(tag::MDEntryType, std::string("0"));
    midCooldownTick.set(tag::MDEntryPx, std::string("4342.00"));
    midCooldownTick.set(tag::MDEntryType, std::string("1"));
    midCooldownTick.set(tag::MDEntryPx, std::string("4342.02"));
    server.sendToClient(kQuoteConn, midCooldownTick);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 300ms), std::runtime_error);
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);

    // Once the cooldown has elapsed, the next tick lets the next cycle start.
    std::this_thread::sleep_for(600ms);  // 300+300+600 > the 1s cooldown
    FixMessage afterCooldownTick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 6);
    afterCooldownTick.set(tag::Symbol, std::string("1"));
    afterCooldownTick.set(tag::NoMDEntries, int64_t{2});
    afterCooldownTick.set(tag::MDEntryType, std::string("0"));
    afterCooldownTick.set(tag::MDEntryPx, std::string("4342.00"));
    afterCooldownTick.set(tag::MDEntryType, std::string("1"));
    afterCooldownTick.set(tag::MDEntryPx, std::string("4342.02"));
    server.sendToClient(kQuoteConn, afterCooldownTick);

    // The fixed range's Buy Stop (4337) is below the market (4342) here, so
    // it waits for the price; the Sell Stop is what goes out first.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4336.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner exposes snapshot() and routes status lines through setLogHandler") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::mutex logMutex;
    std::vector<std::string> logLines;
    // Must be set before run() -- see setLogHandler's doc comment.
    runner.setLogHandler([&](const std::string& line) {
        std::scoped_lock lock(logMutex);
        logLines.push_back(line);
    });

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    // Quote subscription happens before the engine (and initial orders)
    // is constructed -- see the other TEST_CASE's comment on this.
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // seq1
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // seq2

    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    REQUIRE(snap.activeOrderCount == 2);
    REQUIRE(snap.symbolName == "XAUUSD");
    REQUIRE(snap.symbolId == "1");
    REQUIRE(snap.rangeLower == Catch::Approx(4336.00));
    REQUIRE(snap.rangeUpper == Catch::Approx(4337.00));

    {
        std::scoped_lock lock(logMutex);
        const bool sawLogonLine =
            std::any_of(logLines.begin(), logLines.end(),
                        [](const std::string& l) { return l.find("logged on") != std::string::npos; });
        REQUIRE(sawLogonLine);
    }

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner auto-centers the range on the first live quote when autoRangeWidthPoints is set") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 0.20;
    // The configured static range is deliberately far from the tick this
    // test sends, so passing only proves auto-centering actually
    // overrode it rather than coincidentally matching.
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string("4292.40"));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string("4292.50"));
    server.sendToClient(kQuoteConn, snapshot);

    // Mid = 4292.45; a 0.20-wide range centered there is [4292.35, 4292.55].
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4292.55));

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4292.35));

    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.rangeLower == Catch::Approx(4292.35));
    REQUIRE(snap.rangeUpper == Catch::Approx(4292.55));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

// --- Brain v2 live wiring (docs/brain-v2.md "Live wiring") ---

namespace {

// A trained v2 brain file: every cell tradable at gap choice 2 (4x ATR),
// 1.5x lots, 4 levels, exit 0.5 — unless `tradable` is false, in which
// case every cell is no-trade. Written under a fresh temp directory;
// the caller removes it after the runner is gone.
std::filesystem::path writeTestBrainV2(bool tradable, int shiftFills = 0, double shiftStep = 0.0, int firstSide = 0) {
    muuk::core::BrainV2 brain;
    brain.trained = true;
    brain.volLow = 0.5;
    brain.volHigh = 1.5;
    brain.gapAtrChoices = {2.0, 3.0, 4.0, 6.0, 8.0};
    brain.trainedBars = 1500;
    brain.shiftAfterFills = shiftFills;  // Step 2: 0 = an old file, no shift recorded
    brain.stepAtr = shiftStep;
    brain.cells.assign(81, muuk::core::BrainV2Cell{});
    for (auto& c : brain.cells) {
        c.samples = 100;
        c.firstSide = firstSide;  // Step 3: every cell learns the same first side in this test
        if (tradable) {
            c.best.gapChoice = 2;
            c.best.lotMultiplier = 1.5;
            c.best.maxLevels = 4;
            c.best.exitFactor = 0.5;
            c.meanPnl = 5.0;
            c.score = 5.0;
        }
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-v2-wire-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (tradable ? "brain_trade.txt" : "brain_notrade.txt");
    std::ofstream out(file, std::ios::trunc);
    out << muuk::core::serializeBrainV2(brain);
    return file;
}

// 240 flat one-minute bars (ATR 1.0, no trend): the v2 situation read
// needs 150. Monday 2026-01-05 10:00 UTC, London session.
std::vector<muuk::core::Bar> flatBars() {
    std::vector<muuk::core::Bar> bars;
    const int64_t base = 1767607200000LL;  // 2026-01-05 10:00 UTC
    for (int i = 0; i < 240; ++i) {
        bars.push_back(Bar{base + i * 60'000LL, 4336.50, 4337.00, 4336.00, 4336.50, 20.0});
    }
    return bars;
}

// 240 whipsaw one-minute bars (closes alternate $2, ATR ~2.5, no trend):
// the Kaufman ER over 100 minutes is ~0, so a brain that records erMode 2
// must block the cycle while a brain that records erMode 0 trades it.
// Same Monday 2026-01-05 10:00 UTC base as flatBars().
std::vector<muuk::core::Bar> whipsawBars() {
    std::vector<muuk::core::Bar> bars;
    const int64_t base = 1767607200000LL;  // 2026-01-05 10:00 UTC
    double prev = 4336.50;
    for (int i = 0; i < 240; ++i) {
        const double c = (i % 2 == 0) ? 4335.50 : 4337.50;
        bars.push_back(Bar{base + i * 60'000LL, prev, std::max(prev, c) + 0.25, std::min(prev, c) - 0.25, c, 20.0});
        prev = c;
    }
    return bars;
}

void sendTestQuote(MockFixServer& server, const char* bid, const char* ask) {
    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string(bid));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string(ask));
    server.sendToClient(kQuoteConn, snapshot);
}

FixMessage fillMessage(const std::string& clOrdId, const char* orderId, const char* avgPx, const char* qty,
                       int seqNum) {
    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, seqNum);
    fill.set(tag::OrderID, std::string(orderId));
    fill.set(tag::ClOrdID, clOrdId);
    fill.set(tag::PosMaintRptID, "P" + clOrdId);
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string(avgPx));
    fill.set(tag::LastQty, std::string(qty));
    fill.set(tag::CumQty, std::string(qty));
    fill.set(tag::LeavesQty, std::string("0"));
    return fill;
}

// The logon / symbol / quote-subscription preamble every runner test
// needs, up to the MarketDataRequest.
void connectRunner(MockFixServer& server) {
    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);
}

}  // namespace

TEST_CASE("LiveRunner with Brain v2 opens the first range at the brain's gap and lot multiplier") {
    const std::filesystem::path brainFile = writeTestBrainV2(true);
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;  // wait for a live quote to center on
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);

    // Mid 4336.51; the brain's gap is 4.00 (4x ATR 1.00), so the range is
    // [4334.51, 4338.51] — not the configured [4336.00, 4337.00].
    sendTestQuote(server, "4336.50", "4336.52");

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE(order1.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4334.51));
    REQUIRE(order2.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    // The brain's lot multiplier (1.5x) shows in the re-arm: after both
    // boundary orders fill, the next Buy is level 3 = 0.01 x 1.5^2 = 0.02
    // lot (OrderQty 2.0); the configured add-one-lot progression would
    // give 0.03 (OrderQty 3.0).
    server.sendToClient(kTradeConn, fillMessage(order1ClOrdId, "9001", "4338.51", "0.01", 3));
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);

    server.sendToClient(kTradeConn, fillMessage(order2ClOrdId, "9002", "4334.51", "0.02", 4));
    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order3.getInt(tag::Side) == 1);
    REQUIRE(order3.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE(order3.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    REQUIRE(runner.snapshot().brainStatus.find("passed its test") != std::string::npos);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}

// Drives one brain cycle to its third order: quote, both boundary orders,
// both fills, then the re-armed buy. The config shift is off unless the
// caller sets it; the expected third-order price proves which shift ran.
void driveBrainShiftCycle(MockFixServer& server, const std::filesystem::path& brainFile, int configShiftFills,
                          double configShiftStep, double expectedThirdBuy, bool expectShiftText,
                          const std::string& expectedShiftLog) {
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;  // wait for a live quote to center on
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    cfg.strategy.shiftAfterFills = configShiftFills;
    cfg.strategy.shiftStepPoints = configShiftStep;
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::mutex logMutex;
    std::vector<std::string> logLines;
    runner.setLogHandler([&](const std::string& line) {
        std::scoped_lock lock(logMutex);
        logLines.push_back(line);
    });

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);
    sendTestQuote(server, "4336.50", "4336.52");

    // Mid 4336.51; the brain's gap is 4.00 (4x ATR 1.00).
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4334.51));
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    server.sendToClient(kTradeConn, fillMessage(order1ClOrdId, "9001", "4338.51", "0.01", 3));
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);

    // The second fill is the one that triggers the shift.
    server.sendToClient(kTradeConn, fillMessage(order2ClOrdId, "9002", "4334.51", "0.02", 4));
    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order3.getInt(tag::Side) == 1);
    REQUIRE(order3.getDouble(tag::StopPx) == Catch::Approx(expectedThirdBuy));

    const std::string detail = runner.snapshot().brainDetail;
    if (expectShiftText) {
        REQUIRE(detail.find("shift ") != std::string::npos);
    } else {
        REQUIRE(detail.find("shift ") == std::string::npos);
    }
    {
        std::scoped_lock lock(logMutex);
        const bool sawShiftLog =
            std::any_of(logLines.begin(), logLines.end(), [&](const std::string& l) {
                return l.find(expectedShiftLog) != std::string::npos;
            });
        REQUIRE(sawShiftLog);
    }

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}

TEST_CASE("LiveRunner with Brain v2 uses the brain file's shift for the cycle") {
    // Brain shift 2 fills x 2.0 ATR; the flat bars' ATR is 1.00, so the
    // step is $2.00: after the 2nd fill the 4.00 gap widens to 6.00 around
    // the same center 4336.51, and the re-armed buy sits at 4339.51.
    // The config shift is off (testConfig) -- any widening is the brain's.
    const std::filesystem::path brainFile = writeTestBrainV2(true, 2, 2.0);
    MockFixServer server;
    driveBrainShiftCycle(server, brainFile, 0, 1.0, 4339.51, true, "[brain v2] shift from brain: 2 fills x 2.0 ATR");
}

TEST_CASE("LiveRunner with Brain v2 falls back to the config shift when the brain file has none") {
    // An old brain file (no shift fields) with the config shift on: 2 fills
    // x $1.50 widens the 4.00 gap to 5.50, so the re-armed buy sits at
    // 4336.51 + 2.75 = 4339.26 -- and the Brain line shows no shift.
    // ($1.50, not $0.50: the engine floors every widening at $1.00.)
    const std::filesystem::path brainFile = writeTestBrainV2(true);
    MockFixServer server;
    driveBrainShiftCycle(server, brainFile, 2, 1.5, 4339.26, false, "[brain v2] cycle:");
}

TEST_CASE("LiveRunner with Brain v2 opens no range when the brain says no-trade") {
    const std::filesystem::path brainFile = writeTestBrainV2(false);
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::mutex logMutex;
    std::vector<std::string> logLines;
    runner.setLogHandler([&](const std::string& line) {
        std::scoped_lock lock(logMutex);
        logLines.push_back(line);
    });

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);
    sendTestQuote(server, "4336.50", "4336.52");

    // The brain vetoes the first cycle, and keeps vetoing it: no boundary
    // order ever goes out (waitForMessage throws on timeout).
    REQUIRE_THROWS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 2s));
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    {
        std::scoped_lock lock(logMutex);
        const bool sawVeto = std::any_of(logLines.begin(), logLines.end(), [](const std::string& l) {
            return l.find("[brain v2] no new cycle") != std::string::npos;
        });
        REQUIRE(sawVeto);
    }

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}

// --- Brain file search, retry and reload (the "no trained brain" fix) ---

// Writes a trained v2 brain file at `file`: every cell tradable at gap
// choice 2 (4x ATR), 1.5x lots, 4 levels, exit 0.5 when `tradable`,
// no-trade otherwise; `trainedBars` is stamped on the header so a
// re-read can be told apart from the file it replaces.
void writeBrainAt(const std::filesystem::path& file, bool tradable, int64_t trainedBars, int erMode = -1,
                  double stopLoss = 0.0) {
    muuk::core::BrainV2 brain;
    brain.trained = true;
    brain.volLow = 0.5;
    brain.volHigh = 1.5;
    brain.gapAtrChoices = {2.0, 3.0, 4.0, 6.0, 8.0};
    brain.trainedBars = trainedBars;
    brain.erMode = erMode;
    brain.stopLoss = stopLoss;
    brain.cells.assign(81, muuk::core::BrainV2Cell{});
    for (auto& c : brain.cells) {
        c.samples = 100;
        if (tradable) {
            c.best.gapChoice = 2;
            c.best.lotMultiplier = 1.5;
            c.best.maxLevels = 4;
            c.best.exitFactor = 0.5;
            c.meanPnl = 5.0;
            c.score = 5.0;
        }
    }
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::trunc);
    out << muuk::core::serializeBrainV2(brain);
}

// A runner over a temp config folder: the relative brain file, the state
// file under muuk_state/, flat bars, and the runner thread already
// running. The caller drives the mock server (connectRunner +
// sendTestQuote) and stops the thread with finish(); the destructor
// stops it too, so a failed REQUIRE never leaves the thread running.
struct BrainPathRunner {
    MockFixServer& server;
    LiveRunner runner;
    std::thread thread_;
    std::exception_ptr error_;

    BrainPathRunner(MockFixServer& s, LiveConfig cfg, const std::filesystem::path& configFile,
                    const std::filesystem::path& stateFile, LiveRunner::LogHandler logHandler = nullptr,
                    std::vector<muuk::core::Bar> bars = {})
        : server(s), runner(std::move(cfg)) {
        runner.setConfigFile(configFile);
        runner.setStateFile(stateFile);
        runner.addHistoryBars(bars.empty() ? flatBars() : bars);
        if (logHandler) runner.setLogHandler(std::move(logHandler));
        thread_ = std::thread([this] {
            try {
                runner.run();
            } catch (...) {
                error_ = std::current_exception();
            }
        });
    }
    ~BrainPathRunner() { finish(); }
    BrainPathRunner(const BrainPathRunner&) = delete;
    BrainPathRunner& operator=(const BrainPathRunner&) = delete;
    void finish() {
        runner.stop();
        if (thread_.joinable()) thread_.join();
    }
    void rethrow() {
        if (error_) std::rethrow_exception(error_);
    }
};

TEST_CASE("brainFileCandidates searches next to the config file first, then the state file, then the working directory") {
    // Paths are built from temp_directory_path() so the expectations are
    // absolute on every platform: a "/tmp/..." literal resolves against
    // the current drive on Windows and the comparison fails there.
    const auto acct = std::filesystem::temp_directory_path() / "muuk-test-acct";
    const auto config = acct / "my_account.local.json";
    const auto state = acct / "muuk_state" / "my_account.state.json";
    const auto paths = brainFileCandidates(config, state, "brain_v2.txt");
    REQUIRE(paths.size() == 3);
    REQUIRE(paths[0] == acct / "brain_v2.txt");
    REQUIRE(paths[1] == acct / "muuk_state" / "brain_v2.txt");
    REQUIRE(paths[2] == std::filesystem::path("brain_v2.txt"));
    // An absolute brainFile is used as-is, alone. (A "/elsewhere/..."
    // literal is NOT absolute on Windows — root directory without a
    // drive — so the absolute path is built from the temp directory.)
    const auto elsewhere = std::filesystem::temp_directory_path() / "elsewhere" / "custom.txt";
    const auto absolute = brainFileCandidates(config, state, elsewhere.string());
    REQUIRE(absolute.size() == 1);
    REQUIRE(absolute[0] == elsewhere);
    REQUIRE(brainFileCandidates(acct / "x" / "c.json", acct / "x" / "muuk_state" / "c.state.json", "").empty());
    // Config in the working directory: the config-dir and working-directory
    // candidates are the same file, so it appears once.
    const auto deduped =
        brainFileCandidates("my_account.local.json", "muuk_state/my_account.state.json", "brain_v2.txt");
    REQUIRE(deduped.size() == 2);
    REQUIRE(deduped[0] == std::filesystem::current_path() / "brain_v2.txt");
    REQUIRE(deduped[1] == std::filesystem::path("muuk_state/brain_v2.txt"));
}

TEST_CASE("LiveRunner finds the trained brain next to the config file") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-path-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    writeBrainAt(dir / "brain_v2.txt", true, 1500);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json");
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    // The brain's gap is 4.00 (4x ATR 1.00): the range is [4334.51, 4338.51].
    const FixMessage order1 = bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE(bot.runner.snapshot().brainStatus.find("passed its test") != std::string::npos);
    REQUIRE(bot.runner.snapshot().brainStatus.find("1500 bars") != std::string::npos);

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner uses the brain file's recorded erMode even when the panel says off") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-ermode-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    // The brain trained under erMode 2 (chop-block + clean-trend override);
    // the panel still says off (erMode 0): the brain's mode must win, like
    // its trained shift. The brain sits next to the config file only --
    // no muuk_state copy -- so this also re-proves the config-dir-first load.
    writeBrainAt(dir / "brain_v2.txt", true, 1500, 2, 50.0);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.erMode = 0;
    cfg.brain.brainFile = "brain_v2.txt";
    std::mutex logMutex;
    std::vector<std::string> logLines;
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json",
                        [&](const std::string& line) {
                            std::scoped_lock lock(logMutex);
                            logLines.push_back(line);
                        },
                        whipsawBars());
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    // Choppy bars (ER ~ 0): the recorded erMode 2 blocks the cycle, so no
    // boundary order ever goes out (waitForMessage throws on timeout).
    REQUIRE_THROWS(bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 2s));
    REQUIRE(bot.runner.snapshot().status == EngineStatus::Idle);
    {
        std::scoped_lock lock(logMutex);
        const bool sawChop = std::any_of(logLines.begin(), logLines.end(), [](const std::string& l) {
            return l.find("choppy market") != std::string::npos;
        });
        REQUIRE(sawChop);
    }

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner with a brain that records erMode 0 trades the same choppy bars") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-ermode0-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    // The same whipsaw bars, but the brain records erMode 0: no chop-block,
    // so the range opens (gap 4 x ATR ~2.5 = 10.00 around the live mid
    // 4336.51, so one stop is 4341.51 and the other 4331.51).
    writeBrainAt(dir / "brain_v2.txt", true, 1500, 0, 50.0);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.erMode = 0;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json", nullptr, whipsawBars());
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    const FixMessage order1 = bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    const double px = order1.getDouble(tag::StopPx);
    REQUIRE((px == Catch::Approx(4341.51) || px == Catch::Approx(4331.51)));

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner shows the brain file's recorded erMode on the Brain line") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-ermode-line-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    writeBrainAt(dir / "brain_v2.txt", true, 1500, 2, 50.0);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.erMode = 0;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json");
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    const FixMessage order1 = bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    bool sawErMode = false;
    for (int i = 0; i < 40 && !sawErMode; ++i) {
        sawErMode = bot.runner.snapshot().brainDetail.find("erMode 2") != std::string::npos;
        if (!sawErMode) std::this_thread::sleep_for(50ms);
    }
    REQUIRE(sawErMode);

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner still finds the trained brain next to the state file") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-state-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    writeBrainAt(dir / "muuk_state" / "brain_v2.txt", true, 1500);  // only here, not next to the config

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json");
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    const FixMessage order1 = bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE(bot.runner.snapshot().brainStatus.find("passed its test") != std::string::npos);

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner reports every searched path when no trained brain exists") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-nofile-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = "brain_v2.txt";
    std::mutex logMutex;
    std::vector<std::string> logLines;
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json", [&](const std::string& line) {
                            std::scoped_lock lock(logMutex);
                            logLines.push_back(line);
                        });
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    bool sawSearch = false;
    for (int i = 0; i < 40 && !sawSearch; ++i) {
        std::this_thread::sleep_for(50ms);
        std::scoped_lock lock(logMutex);
        sawSearch = std::any_of(logLines.begin(), logLines.end(), [&](const std::string& l) {
            return l.find("no trained brain -- looked in") != std::string::npos &&
                   l.find("muuk_state") != std::string::npos;
        });
    }
    REQUIRE(sawSearch);
    REQUIRE(bot.runner.snapshot().brainStatus == "no trained brain");

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("refreshBrainFile picks up a brain file dropped in after start") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-refresh-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json");
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");
    REQUIRE(bot.runner.snapshot().brainStatus == "no trained brain");

    // The brain file arrives after start: the manual refresh picks it up.
    // (The auto-refresh may win the race and load it on its own tick
    // first -- then the manual refresh reports "already loaded" instead
    // of "reloaded". Either way the brain is loaded without a restart,
    // and the manual re-read happened.)
    writeBrainAt(dir / "brain_v2.txt", true, 1500);
    bot.runner.refreshBrainFile();
    REQUIRE(bot.runner.snapshot().brainStatus.find("passed its test") != std::string::npos);
    REQUIRE(bot.runner.snapshot().brainRefreshNote.find("1500 bars") != std::string::npos);
    // A second refresh with the same file does not rebuild the keeper.
    bot.runner.refreshBrainFile();
    REQUIRE(bot.runner.snapshot().brainRefreshNote.find("already loaded") != std::string::npos);

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner trainBrainNow: a run on too few bars reports it and keeps 'no trained brain'") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-train-now-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json");
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    // A tiny history file: 100 flat bars -- far below the 2000-bar guard.
    {
        std::ofstream out(dir / "tiny.csv");
        out << "# point=0.01,gmtOffset=0\n";
        for (int i = 0; i < 100; ++i) {
            out << (1700000000 + i * 60) << ",4336.50,4336.52,4336.48,4336.50,20,0\n";
        }
    }
    bot.runner.trainBrainNow(dir / "tiny.csv");
    // The run finishes on its own thread: poll the snapshot until it shows
    // the outcome and the progress line has cleared.
    LiveSnapshot snap;
    bool sawResult = false;
    for (int i = 0; i < 200 && !sawResult; ++i) {
        std::this_thread::sleep_for(50ms);
        snap = bot.runner.snapshot();
        sawResult = snap.brainTrainingResult.find("too few bars") != std::string::npos && snap.brainTraining.empty();
    }
    REQUIRE(sawResult);
    REQUIRE(snap.brainStatus == "no trained brain");

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner reloads the brain when its file changes, without a restart") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-reload-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    writeBrainAt(dir / "brain_v2.txt", false, 1500);  // no-trade brain first

    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = "brain_v2.txt";
    BrainPathRunner bot(server, std::move(cfg), dir / "my_account.local.json",
                        dir / "muuk_state" / "my_account.state.json");
    connectRunner(bot.server);
    sendTestQuote(bot.server, "4336.50", "4336.52");

    // The no-trade brain vetoes the first cycle: no boundary order goes out.
    REQUIRE_THROWS(bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 2s));
    REQUIRE(bot.runner.snapshot().brainStatus.find("1500 bars") != std::string::npos);

    // The brain file is replaced by a tradable one: the next quote reloads it.
    writeBrainAt(dir / "brain_v2.txt", true, 2500);
    sendTestQuote(bot.server, "4336.50", "4336.52");
    const FixMessage order1 = bot.server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    // StopPx is the brain's gap (4x ATR) around the live mid 4336.51:
    // 4338.51 when ATR is exactly 1.00, 4338.37 when the two quotes
    // straddle a wall-clock minute boundary. Then the first quote's
    // forming bar (open = high = low = close = 4336.51) closes with true
    // range 0.01 against the previous close 4336.50, so ATR(14) =
    // (13 x 1.00 + 0.01) / 14 = 0.9293, gap 4.0 x 0.9293 = 3.72, and the
    // buy stop is 4336.51 + 1.86 = 4338.37. Both values still prove the
    // reload: had the cycle fallen back to the configured width (1.00),
    // the buy stop would be 4336.51 + 0.50 = 4337.01, which neither
    // tolerated value equals. (The order's existence alone already proves
    // the reload -- the no-trade brain vetoes -- and the "2500 bars"
    // check below pins the reloaded file.)
    const double stopPx = order1.getDouble(tag::StopPx);
    REQUIRE((stopPx == Catch::Approx(4338.51) || stopPx == Catch::Approx(4338.37)));
    REQUIRE(bot.runner.snapshot().brainStatus.find("2500 bars") != std::string::npos);

    bot.finish();
    bot.rethrow();
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("loadLiveConfig reads the brain section, with safe defaults when it is missing") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "muuk-brain-v2-config-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "config.json";
    const std::string strategy = R"JSON("strategy": {
      "symbol": {"name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0,
                 "contractSize": 100.0, "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0},
      "range": {"lower": 4336.00, "upper": 4337.00},
      "lotProgression": {"initialLot": 0.01, "lotIncrement": 0.01},
      "riskLimits": {"maxOrders": 20, "maxLot": 1.0},
      "costs": {}
    })JSON";
    {
        std::ofstream out(file, std::ios::trunc);
        out << "{\"connection\": {"
               "\"quote\": {\"host\": \"127.0.0.1\", \"port\": \"5000\"},"
               "\"trade\": {\"host\": \"127.0.0.1\", \"port\": \"5001\"},"
               "\"environment\": \"demo\", \"brokerUid\": \"b\", \"traderLogin\": \"1\", \"password\": \"p\","
               "\"symbolName\": \"XAUUSD\"},"
            << strategy << ","
            << "\"brain\": {\"enabled\": false, \"brainFile\": \"custom_brain.txt\", \"allowRealAccount\": true}}";
    }
    const LiveConfig withBrain = loadLiveConfig(file);
    REQUIRE(withBrain.brain.enabled == false);
    REQUIRE(withBrain.brain.brainFile == "custom_brain.txt");
    REQUIRE(withBrain.brain.allowRealAccount == true);

    {
        std::ofstream out(file, std::ios::trunc);
        out << "{\"connection\": {"
               "\"quote\": {\"host\": \"127.0.0.1\", \"port\": \"5000\"},"
               "\"trade\": {\"host\": \"127.0.0.1\", \"port\": \"5001\"},"
               "\"environment\": \"demo\", \"brokerUid\": \"b\", \"traderLogin\": \"1\", \"password\": \"p\","
               "\"symbolName\": \"XAUUSD\"},"
            << strategy << "}";
    }
    const LiveConfig defaults = loadLiveConfig(file);
    REQUIRE(defaults.brain.enabled == true);
    REQUIRE(defaults.brain.brainFile == "brain_v2.txt");
    REQUIRE(defaults.brain.allowRealAccount == false);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("LiveRunner with Brain v2 works on a cent-style symbol (money in cents)") {
    // MT5 cent shape: tick value 100 USC per 0.01 tick, so 10,000 USC per
    // lot per $1.00 move (see the ProCent gateway test). The brain's gap
    // and lot multiplier drive the same range and orders as on a standard
    // account; only the money the brain is shown is scaled (replay units),
    // and that scaling is not observable through the public API while the
    // money caps are off by default — this test pins the shape.
    const std::filesystem::path brainFile = writeTestBrainV2(true);
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.strategy.symbol.tickValue = 100.0;  // cent account: money in USC
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);
    sendTestQuote(server, "4336.50", "4336.52");

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE(order1.getDouble(tag::OrderQty) == Catch::Approx(1.0));

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4334.51));
    REQUIRE(order2.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    REQUIRE(runner.snapshot().brainStatus.find("passed its test") != std::string::npos);
    REQUIRE(runner.snapshot().moneyPerPointPerLot == Catch::Approx(10000.0));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}

TEST_CASE("LiveRunner on MT5 ignores a stale stored symbol id and resolves by name") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.platform = "mt5";
    // A cTrader-era symbol id left in the config (the v0.2.3 live bug):
    // on MT5 the bridge always reports the symbol as "1", so trusting
    // this id made the runner drop its own positions and orders as
    // "other symbols" and declare every fill "closed outside the bot".
    cfg.connection.symbolId = "41";
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

    // The bridge sends the MT5 account figures right after logon; the
    // runner waits for them before resolving the symbol.
    FixMessage account = serverHeaderMessage(msgtype::MuukAccountInfo, 2);
    account.set(tag::MuukLogin, std::string("12345"));
    account.set(tag::MuukServer, std::string("Test-Demo"));
    account.set(tag::MuukCompany, std::string("Test Markets Ltd."));
    account.set(tag::MuukCurrency, std::string("USC"));
    account.set(tag::MuukTradeMode, std::string("demo"));
    account.set(tag::MuukSymbol, std::string("XAUUSD"));
    account.set(tag::MuukBalance, std::string("500000"));
    account.set(tag::MuukEquity, std::string("500000"));
    account.set(tag::MuukLeverage, std::string("1000"));
    account.set(tag::MuukHedging, std::string("Y"));
    account.set(tag::MuukDigits, std::string("2"));
    account.set(tag::MuukTickSize, std::string("0.01"));
    account.set(tag::MuukTickValue, std::string("1"));
    account.set(tag::MuukVolumeMin, std::string("0.01"));
    account.set(tag::MuukVolumeStep, std::string("0.01"));
    server.sendToClient(kTradeConn, account);

    // The runner must ask the SecurityList instead of trusting "41":
    // had it used the stored id, no request would ever arrive.
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    // The gateway's id for XAUUSD is "1", not the stale "41".
    for (int i = 0; i < 100 && runner.snapshot().symbolId != "1"; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(runner.snapshot().symbolId == "1");

    // The orders it arms carry the resolved id too.
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order1.getString(tag::Symbol) == "1");
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner with Brain v2 arms only the brain's first side (Sell first)") {
    // The brain says Sell first, so only the Sell Stop arms -- no Buy Stop
    // follows, and the Brain line says so. The config default would arm
    // the Buy Stop first, so this proves the brain overrides it.
    const std::filesystem::path brainFile = writeTestBrainV2(true, 0, 0.0, -1);
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;  // wait for a live quote to center on
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);

    // Mid 4336.51; the brain's gap is 4.00, so the range is [4334.51, 4338.51].
    sendTestQuote(server, "4336.50", "4336.52");

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 2);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4334.51));
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 800ms), std::runtime_error);
    REQUIRE(runner.snapshot().brainDetail.find("first Sell") != std::string::npos);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}

TEST_CASE("LiveRunner with Brain v2 arms only the brain's first side (Buy first)") {
    const std::filesystem::path brainFile = writeTestBrainV2(true, 0, 0.0, 1);
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);
    sendTestQuote(server, "4336.50", "4336.52");

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 800ms), std::runtime_error);
    REQUIRE(runner.snapshot().brainDetail.find("first Buy") != std::string::npos);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}

TEST_CASE("LiveRunner with Brain v2 is not blocked by the trend filter") {
    // A trained brain decides its own cycles: with trendMin set and flat
    // bars (no trend at all), the standalone trend filter would hold a
    // config cycle -- but the brain cycle still opens. Without a brain the
    // filter still holds (see the Smart entry test in test_live_resume.cpp).
    const std::filesystem::path brainFile = writeTestBrainV2(true);
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 1.0;
    cfg.connection.entry.trendMin = 1.0;
    cfg.brain.enabled = true;
    cfg.brain.brainFile = brainFile.string();
    LiveRunner runner(cfg);
    runner.addHistoryBars(flatBars());

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};

    connectRunner(server);
    sendTestQuote(server, "4336.50", "4336.52");

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4338.51));
    REQUIRE(runner.snapshot().entryWait.empty());

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
    std::error_code ec;
    std::filesystem::remove_all(brainFile.parent_path(), ec);
}
