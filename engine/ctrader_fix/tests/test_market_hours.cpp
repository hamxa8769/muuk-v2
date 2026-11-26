#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/market_hours.hpp"
#include "muuk/ctrader_fix/news_feed.hpp"

using namespace muuk::ctrader_fix;

namespace {
int64_t utc(int y, int mo, int d, int h, int mi) { return utcMsFromCivil(y, mo, d, h, mi, 0); }
MarketStatus at(int y, int mo, int d, int h, int mi, const MarketHoursConfig& c = {}) {
    return evaluateMarket(utc(y, mo, d, h, mi), c, nullptr, false);
}
const HolidayEvent* findHoliday(const std::vector<HolidayEvent>& list, int mo, int d) {
    for (const HolidayEvent& e : list) {
        if (e.month == mo && e.day == d) return &e;
    }
    return nullptr;
}
}  // namespace

TEST_CASE("Daylight saving: New York, London and Sydney switch on the right days") {
    // 2026: US 8 Mar / 1 Nov, UK 29 Mar / 25 Oct, Sydney ends 5 Apr, starts 4 Oct.
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 3, 8, 6, 59)) == -300);
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 3, 8, 7, 0)) == -240);
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 11, 1, 5, 59)) == -240);
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 11, 1, 6, 0)) == -300);
    REQUIRE(sessionUtcOffsetMinutes(Session::London, utc(2026, 3, 29, 0, 59)) == 0);
    REQUIRE(sessionUtcOffsetMinutes(Session::London, utc(2026, 3, 29, 1, 0)) == 60);
    REQUIRE(sessionUtcOffsetMinutes(Session::London, utc(2026, 10, 25, 1, 0)) == 0);
    REQUIRE(sessionUtcOffsetMinutes(Session::Sydney, utc(2026, 4, 4, 15, 59)) == 660);
    REQUIRE(sessionUtcOffsetMinutes(Session::Sydney, utc(2026, 4, 4, 16, 0)) == 600);
    REQUIRE(sessionUtcOffsetMinutes(Session::Sydney, utc(2026, 10, 3, 16, 0)) == 660);
    REQUIRE(sessionUtcOffsetMinutes(Session::Tokyo, utc(2026, 7, 1, 0, 0)) == 540);
    REQUIRE(newYorkWeekday(utc(2026, 9, 25, 12, 0)) == 5);  // Friday
}

TEST_CASE("Sessions open on their own city's clock") {
    // Thursday 24 Sep 2026 (summer): London 07-16 UTC, New York 12-21 UTC.
    REQUIRE(sessionLabelAt(utc(2026, 9, 24, 13, 0)) == "London + New York");
    REQUIRE(sessionLabelAt(utc(2026, 9, 24, 3, 0)) == "Sydney + Tokyo");
    REQUIRE(sessionLabelAt(utc(2026, 9, 24, 21, 30)) == "Market closed");  // daily break
    REQUIRE(sessionLabelAt(utc(2026, 9, 26, 12, 0)) == "Market closed");   // Saturday
}

TEST_CASE("Sessions ending don't pause anything -- only gold's own close and reopen do") {
    // London closes 17:00 BST = 16:00 UTC, Tokyo 18:00 JST = 09:00 UTC: trading goes on.
    REQUIRE(at(2026, 9, 24, 15, 55).phase == MarketPhase::Open);
    REQUIRE(at(2026, 9, 23, 8, 55).phase == MarketPhase::Open);
    // Gold closes 17:00 EDT = 21:00 UTC: no new range in the 15 min before.
    REQUIRE(at(2026, 9, 24, 20, 44).phase == MarketPhase::Open);
    const MarketStatus p = at(2026, 9, 24, 20, 50);
    REQUIRE(p.phase == MarketPhase::SessionPause);
    REQUIRE(p.label == "Pause: market closing");
    REQUIRE_FALSE(p.canOpenRange);
    // Switched off: no pause before the close.
    MarketHoursConfig c;
    c.pauseAroundClose = false;
    REQUIRE(at(2026, 9, 24, 20, 50, c).phase == MarketPhase::Open);
    REQUIRE(at(2026, 9, 24, 22, 0, c).phase == MarketPhase::Open);  // nor right after the reopen
}

TEST_CASE("Gold's daily break, the reopening minute and the weekend") {
    // New York close 17:00 EDT = 21:00 UTC: pause before, then the break.
    REQUIRE(at(2026, 9, 24, 20, 50).label == "Pause: market closing");
    const MarketStatus brk = at(2026, 9, 24, 21, 30);
    REQUIRE(brk.phase == MarketPhase::DailyBreak);
    REQUIRE_FALSE(brk.marketOpen);
    REQUIRE(at(2026, 9, 24, 22, 0).label == "Pause: market reopening");
    REQUIRE(at(2026, 9, 24, 22, 1).phase == MarketPhase::Open);
    // Winter: 17:00 EST = 22:00 UTC.
    REQUIRE(at(2026, 12, 15, 22, 30).phase == MarketPhase::DailyBreak);
    REQUIRE(at(2026, 12, 15, 21, 30).phase == MarketPhase::Open);
    // Friday 17:00 New York to Sunday 18:00.
    REQUIRE(at(2026, 9, 25, 21, 0).phase == MarketPhase::Weekend);
    REQUIRE(at(2026, 9, 26, 12, 0).phase == MarketPhase::Weekend);
    REQUIRE(at(2026, 9, 27, 21, 59).phase == MarketPhase::Weekend);
    REQUIRE(at(2026, 9, 27, 22, 1).phase == MarketPhase::Open);
    // Filter off: never blocks, but still says what the market is doing.
    MarketHoursConfig off;
    off.enabled = false;
    const MarketStatus wk = at(2026, 9, 26, 12, 0, off);
    REQUIRE(wk.phase == MarketPhase::Weekend);
    REQUIRE(wk.canOpenRange);
}

TEST_CASE("Every session is traded, also between sessions") {
    REQUIRE(at(2026, 9, 24, 3, 0).phase == MarketPhase::Open);    // Sydney + Tokyo
    REQUIRE(at(2026, 9, 24, 10, 0).phase == MarketPhase::Open);   // London
    REQUIRE(at(2026, 9, 24, 23, 30).phase == MarketPhase::Open);  // between New York and Sydney
    // Friday: the pause before the weekend close.
    REQUIRE(at(2026, 9, 25, 20, 50).label == "Pause: market closing");
}

TEST_CASE("The next change is found, with its reason") {
    const MarketStatus s = evaluateMarket(utc(2026, 9, 24, 15, 0), MarketHoursConfig{});
    REQUIRE(s.canOpenRange);
    REQUIRE(s.nextChangeUtcMs == utc(2026, 9, 24, 20, 45));
    REQUIRE(s.nextChangeLabel == "Pause: market closing");
    const MarketStatus w = evaluateMarket(utc(2026, 9, 26, 12, 0), MarketHoursConfig{});
    REQUIRE(w.nextChangeUtcMs == utc(2026, 9, 27, 22, 1));
    REQUIRE(w.nextChangeLabel == "Trading resumes");
}

TEST_CASE("ForexFactory calendar: parsed to UTC, and the news guard follows impact, currency and the toggle") {
    const std::string json = R"([
        {"title":"Non-Farm Employment Change","country":"USD","date":"2026-10-02T08:30:00-04:00","impact":"High","forecast":"150K","previous":"142K"},
        {"title":"ISM Services PMI","country":"USD","date":"2026-10-05T10:00:00-04:00","impact":"Medium","forecast":"","previous":""},
        {"title":"German CPI","country":"EUR","date":"2026-10-02T08:00:00+02:00","impact":"High","forecast":"","previous":""},
        {"title":"Bank Holiday","country":"GBP","date":"2026-10-02T03:00:00-04:00","impact":"Holiday","forecast":"","previous":""},
        {"title":"broken","country":"USD","date":"soon","impact":"High"}
    ])";
    const std::vector<NewsEvent> feed = parseForexFactoryCalendar(json);
    REQUIRE(feed.size() == 4);
    REQUIRE(feed[0].utcMs == utc(2026, 10, 2, 12, 30));
    REQUIRE(feed[0].impact == "High");
    REQUIRE(feed[0].forecast == "150K");
    REQUIRE(feed[2].utcMs == utc(2026, 10, 2, 6, 0));

    MarketHoursConfig c;  // guard on, High only, USD
    REQUIRE(newsIsGuarded(feed[0], c));
    REQUIRE_FALSE(newsIsGuarded(feed[1], c));  // Medium: off by default
    REQUIRE_FALSE(newsIsGuarded(feed[2], c));  // EUR
    c.newsMedium = true;
    REQUIRE(newsIsGuarded(feed[1], c));
    c.newsMedium = false;

    // NFP 12:30 UTC: guarded 12:15 .. 12:45.
    const MarketStatus before = evaluateMarket(utc(2026, 10, 2, 12, 10), c, &feed);
    REQUIRE(before.canOpenRange);
    REQUIRE(before.hasNextNews);
    REQUIRE(before.nextNews.title == "Non-Farm Employment Change");
    REQUIRE(before.nextChangeUtcMs == utc(2026, 10, 2, 12, 15));
    const MarketStatus during = evaluateMarket(utc(2026, 10, 2, 12, 20), c, &feed);
    REQUIRE(during.phase == MarketPhase::NewsPause);
    REQUIRE(during.label == "News guard: USD Non-Farm Employment Change");
    REQUIRE(evaluateMarket(utc(2026, 10, 2, 12, 45), c, &feed, false).phase == MarketPhase::Open);

    c.newsGuard = false;
    REQUIRE(evaluateMarket(utc(2026, 10, 2, 12, 20), c, &feed, false).phase == MarketPhase::Open);
}

TEST_CASE("Market hours settings survive a save and load") {
    MarketHoursConfig c;
    c.pauseAroundClose = false;
    c.pauseMinutesBefore = 20;
    c.newsGuard = false;
    c.newsMedium = true;
    c.newsCurrencies = "USD, EUR";
    c.closeRangeAt = "weekend";
    NewsEvent n;
    n.utcMs = utc(2026, 10, 2, 12, 30);
    n.title = "FOMC";
    n.manual = true;
    c.news.push_back(n);
    const MarketHoursConfig back = marketHoursFromJson(marketHoursToJson(c));
    REQUIRE_FALSE(back.pauseAroundClose);
    REQUIRE(back.pauseMinutesBefore == 20);
    REQUIRE_FALSE(back.newsGuard);
    REQUIRE(back.newsMedium);
    REQUIRE(back.newsCurrencies == "USD, EUR");
    REQUIRE(back.closeRangeAt == "weekend");
    REQUIRE(back.news.size() == 1);
    REQUIRE(back.news[0].manual);
    REQUIRE(back.news[0].title == "FOMC");
    // A hand-added event is always guarded (while the guard is on).
    REQUIRE(newsIsGuarded(back.news[0], MarketHoursConfig{}));
}

TEST_CASE("CME holidays: Thanksgiving halt and reopen, with the pause before it") {
    // Thanksgiving 2026: Thu Nov 26, halt 14:30 ET = 19:30 UTC (EST).
    const MarketStatus thanks = at(2026, 11, 26, 19, 30);
    REQUIRE(thanks.phase == MarketPhase::Holiday);
    REQUIRE_FALSE(thanks.marketOpen);
    REQUIRE_FALSE(thanks.canOpenRange);
    REQUIRE(thanks.label.find("Thanksgiving") != std::string::npos);
    REQUIRE(thanks.label.find("halt 14:30 ET") != std::string::npos);
    REQUIRE(thanks.holidayName == "Thanksgiving");
    REQUIRE(thanks.holidayReopenUtcMs == utc(2026, 11, 26, 23, 0));
    REQUIRE(thanks.detail.find("Thanksgiving") != std::string::npos);
    REQUIRE(thanks.detail.find("Reopens 18:00 ET") != std::string::npos);
    // One minute before the halt: pause before the holiday halt.
    const MarketStatus before = at(2026, 11, 26, 19, 29);
    REQUIRE(before.phase == MarketPhase::SessionPause);
    REQUIRE(before.pauseIsHoliday);
    REQUIRE(before.label.find("Thanksgiving") != std::string::npos);
    REQUIRE(before.label.find("holiday halt") != std::string::npos);
    // Reopen 18:00 ET = 23:00 UTC: the 1-min post-reopen pause, then open.
    const MarketStatus reopen = at(2026, 11, 26, 23, 0);
    REQUIRE(reopen.phase == MarketPhase::SessionPause);
    REQUIRE(reopen.label.find("reopening") != std::string::npos);
    REQUIRE(at(2026, 11, 26, 23, 1).phase == MarketPhase::Open);
    // The minute scan finds the holiday transitions too.
    const MarketStatus next = evaluateMarket(utc(2026, 11, 26, 19, 0), MarketHoursConfig{});
    REQUIRE(next.nextChangeUtcMs == utc(2026, 11, 26, 19, 15));
    REQUIRE(next.nextChangeLabel.find("Thanksgiving") != std::string::npos);
    const MarketStatus resume = evaluateMarket(utc(2026, 11, 26, 19, 45), MarketHoursConfig{});
    REQUIRE(resume.nextChangeUtcMs == utc(2026, 11, 26, 23, 1));
    REQUIRE(resume.nextChangeLabel == "Trading resumes");
}

TEST_CASE("CME holidays: Black Friday early close, Christmas, Good Friday, MLK, New Year's Day") {
    // Black Friday 2026: Fri Nov 27, early close 14:45 ET = 19:45 UTC.
    const MarketStatus bf = at(2026, 11, 27, 19, 45);
    REQUIRE(bf.phase == MarketPhase::Holiday);
    REQUIRE(bf.label.find("Black Friday") != std::string::npos);
    REQUIRE(bf.label.find("early close 14:45 ET") != std::string::npos);
    REQUIRE(bf.holidayReopenUtcMs == utc(2026, 11, 29, 23, 0));
    // One minute before the early close: pause before the holiday close.
    const MarketStatus bfPause = at(2026, 11, 27, 19, 44);
    REQUIRE(bfPause.phase == MarketPhase::SessionPause);
    REQUIRE(bfPause.pauseIsHoliday);
    // Saturday: closed either way (weekend, or the holiday window).
    const MarketStatus sat = at(2026, 11, 28, 12, 0);
    REQUIRE(((sat.phase == MarketPhase::Weekend) || (sat.phase == MarketPhase::Holiday)));
    REQUIRE_FALSE(sat.marketOpen);
    REQUIRE(at(2026, 11, 29, 23, 1).phase == MarketPhase::Open);

    // Christmas Eve 2026: Thu Dec 24, early close 13:45 ET = 18:45 UTC.
    const MarketStatus eve = at(2026, 12, 24, 18, 45);
    REQUIRE(eve.phase == MarketPhase::Holiday);
    REQUIRE(eve.label.find("Christmas Eve") != std::string::npos);
    REQUIRE(eve.holidayReopenUtcMs == utc(2026, 12, 27, 23, 0));
    // Christmas Day 2026: Fri Dec 25, closed all day.
    const MarketStatus xmas = at(2026, 12, 25, 12, 0);
    REQUIRE(xmas.phase == MarketPhase::Holiday);
    REQUIRE(xmas.label.find("Christmas Day") != std::string::npos);
    REQUIRE(xmas.label.find("closed") != std::string::npos);
    REQUIRE(xmas.holidayReopenUtcMs == utc(2026, 12, 27, 23, 0));
    REQUIRE(at(2026, 12, 27, 23, 1).phase == MarketPhase::Open);

    // Good Friday 2026: Fri Apr 3, closed all day.
    REQUIRE(at(2026, 4, 3, 12, 0).phase == MarketPhase::Holiday);
    // MLK Day 2026: Mon Jan 19, halt 14:30 ET = 19:30 UTC.
    REQUIRE(at(2026, 1, 19, 19, 30).phase == MarketPhase::Holiday);
    // New Year's Day 2026: Thu Jan 1, closed all day, reopens Thu 18:00 ET.
    const MarketStatus nyd = at(2026, 1, 1, 12, 0);
    REQUIRE(nyd.phase == MarketPhase::Holiday);
    REQUIRE(nyd.holidayReopenUtcMs == utc(2026, 1, 1, 23, 0));
    REQUIRE(std::string(phaseName(MarketPhase::Holiday)) == "holiday");
}

TEST_CASE("Holiday news guard: Holiday impact follows currency and the newsHoliday toggle") {
    NewsEvent us;
    us.utcMs = utc(2026, 9, 24, 12, 0);
    us.title = "Bank Holiday";
    us.currency = "USD";
    us.impact = "Holiday";
    MarketHoursConfig c;  // guard on, USD
    REQUIRE(newsIsGuarded(us, c));
    NewsEvent gbp = us;
    gbp.currency = "GBP";
    REQUIRE_FALSE(newsIsGuarded(gbp, c));  // GBP is not in the default USD-only list
    c.newsHoliday = false;
    REQUIRE_FALSE(newsIsGuarded(us, c));
    // The guard pauses trading around a USD Holiday event while it is on.
    const std::vector<NewsEvent> feed{us};
    const MarketStatus during = evaluateMarket(utc(2026, 9, 24, 12, 5), MarketHoursConfig{}, &feed, false);
    REQUIRE(during.phase == MarketPhase::NewsPause);
    REQUIRE(evaluateMarket(utc(2026, 9, 24, 12, 5), c, &feed, false).phase == MarketPhase::Open);
    // The toggle survives a save and load (on by default).
    const MarketHoursConfig back = marketHoursFromJson(marketHoursToJson(c));
    REQUIRE_FALSE(back.newsHoliday);
    REQUIRE(marketHoursFromJson(marketHoursToJson(MarketHoursConfig{})).newsHoliday);
}

TEST_CASE("Reopen gap and spread-settled helpers") {
    REQUIRE(reopenGapTooBig(3550, 3580, 10));        // 30 > 2 x 10
    REQUIRE_FALSE(reopenGapTooBig(3550, 3565, 10));  // 15 <= 2 x 10
    REQUIRE_FALSE(reopenGapTooBig(3550, 3600, 0));   // no ATR to judge against
    REQUIRE_FALSE(reopenGapTooBig(3550, 3600, -5));
    REQUIRE_FALSE(spreadSettled(0.50, 0.30));  // 0.50 > 1.5 x 0.30
    REQUIRE(spreadSettled(0.40, 0.30));        // 0.40 <= 1.5 x 0.30
    REQUIRE(spreadSettled(0.50, 0.0));         // no median to judge against
}

TEST_CASE("holidaysForYear: computed rules plus the verified 2026-2027 overrides") {
    const std::vector<HolidayEvent> h26 = holidaysForYear(2026);
    REQUIRE(h26.size() == 13);
    const HolidayEvent* thanks = findHoliday(h26, 11, 26);
    REQUIRE(thanks != nullptr);
    REQUIRE(thanks->name == "Thanksgiving");
    REQUIRE(thanks->kind == HolidayKind::Halt);
    REQUIRE(thanks->haltMinuteET == 870);
    REQUIRE(thanks->reopenMinuteET == 1080);
    REQUIRE(thanks->reopenDayOffset == 0);
    const HolidayEvent* bf = findHoliday(h26, 11, 27);
    REQUIRE(bf != nullptr);
    REQUIRE(bf->name == "Black Friday");
    REQUIRE(bf->kind == HolidayKind::EarlyClose);
    REQUIRE(bf->haltMinuteET == 885);
    REQUIRE(bf->reopenDayOffset == 2);
    // Juneteenth 2026 falls on a Friday: 13:00 ET halt, reopen Sunday.
    const HolidayEvent* june = findHoliday(h26, 6, 19);
    REQUIRE(june != nullptr);
    REQUIRE(june->kind == HolidayKind::EarlyClose);
    REQUIRE(june->haltMinuteET == 780);
    // Independence Day 2026 falls on a Saturday: observed Fri Jul 3.
    const HolidayEvent* july = findHoliday(h26, 7, 3);
    REQUIRE(july != nullptr);
    REQUIRE(july->kind == HolidayKind::EarlyClose);
    REQUIRE(july->haltMinuteET == 780);
    REQUIRE(findHoliday(h26, 7, 4) == nullptr);

    const std::vector<HolidayEvent> h27 = holidaysForYear(2027);
    REQUIRE(h27.size() == 12);
    const HolidayEvent* thanks27 = findHoliday(h27, 11, 25);
    REQUIRE(thanks27 != nullptr);
    REQUIRE(thanks27->name == "Thanksgiving");
    REQUIRE(thanks27->kind == HolidayKind::Halt);
    REQUIRE(thanks27->haltMinuteET == 870);
    // Christmas 2027 falls on a Saturday: observed Fri Dec 24, closed all day.
    const HolidayEvent* xmas27 = findHoliday(h27, 12, 24);
    REQUIRE(xmas27 != nullptr);
    REQUIRE(xmas27->kind == HolidayKind::Closed);
    REQUIRE(xmas27->haltMinuteET == -1);
    REQUIRE(xmas27->name.find("Christmas") != std::string::npos);
    REQUIRE(findHoliday(h27, 12, 25) == nullptr);
    // Good Friday 2027 (Easter Mar 28) and Juneteenth observed Fri Jun 18.
    const HolidayEvent* gf27 = findHoliday(h27, 3, 26);
    REQUIRE(gf27 != nullptr);
    REQUIRE(gf27->kind == HolidayKind::Closed);
    REQUIRE(gf27->reopenDayOffset == 2);
    const HolidayEvent* june27 = findHoliday(h27, 6, 18);
    REQUIRE(june27 != nullptr);
    REQUIRE(june27->kind == HolidayKind::EarlyClose);
    REQUIRE(june27->haltMinuteET == 780);
    // No holiday on an ordinary day.
    REQUIRE_FALSE(holidayAt(utc(2026, 9, 24, 12, 0)).has_value());
    REQUIRE(holidayAt(utc(2026, 11, 26, 12, 0))->name == "Thanksgiving");
}
