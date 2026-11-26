#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace muuk::ctrader_fix {

// The four FX trading sessions, each on its own city's clock (daylight
// saving included): Sydney 07-16, Tokyo 09-18, London 08-17, New York
// 08-17, Monday to Friday.
enum class Session { Sydney = 0, Tokyo = 1, London = 2, NewYork = 3 };
inline constexpr int kSessionCount = 4;
[[nodiscard]] const char* sessionName(Session s);

// A news release to stay out of, e.g. NFP -- from the ForexFactory
// calendar, or added by hand.
struct NewsEvent {
    int64_t utcMs = 0;     // release time
    std::string title;
    std::string currency;  // "USD"...
    std::string impact;    // "High" (red), "Medium" (orange), "Low", "Holiday"; manual entries: "High"
    std::string forecast;
    std::string previous;
    bool manual = false;
};

// When the bot may open a new range. A range already running keeps being
// managed through any pause (profit target and stop loss still work);
// only new ranges wait -- unless closeRangeAt says otherwise.
struct MarketHoursConfig {
    bool enabled = true;
    // Pause around gold's own close and reopen only (17:00 / 18:00 New York:
    // the daily break, and the weekend): no new range from pauseMinutesBefore
    // before it closes, nor in the first pauseMinutesAfter after it reopens --
    // spreads widen and prices jump there. The trading sessions (Sydney,
    // Tokyo, London, New York) opening and closing don't pause anything.
    bool pauseAroundClose = true;
    int pauseMinutesBefore = 15;
    int pauseMinutesAfter = 1;
    // News guard: pause newsMinutesBefore .. newsMinutesAfter around each
    // guarded release -- ForexFactory events of the ticked impacts in the
    // listed currencies, plus the ones added by hand in `news`.
    bool newsGuard = true;
    bool newsHigh = true;       // red folder
    bool newsMedium = false;    // orange folder
    bool newsHoliday = true;    // "Holiday" impact: bank holidays, CME closures
    std::string newsCurrencies = "USD";  // comma separated; gold moves on USD news
    std::vector<NewsEvent> news;         // added by hand
    int newsMinutesBefore = 15;
    int newsMinutesAfter = 15;
    // "never": a running range is left to finish; "weekend": close it before
    // the Friday close; "every": close it whenever a pause starts.
    std::string closeRangeAt = "never";
};

[[nodiscard]] nlohmann::json marketHoursToJson(const MarketHoursConfig& c);
[[nodiscard]] MarketHoursConfig marketHoursFromJson(const nlohmann::json& j);

enum class MarketPhase {
    Open,          // new ranges may open
    Weekend,       // XAUUSD closed: Friday 17:00 to Sunday 18:00 New York time
    DailyBreak,    // XAUUSD's daily break, 17:00-18:00 New York time
    SessionPause,  // around a session's end
    NewsPause,     // around a news release
    OffHours,      // (no longer used: every session is traded)
    Holiday,       // CME metals holiday: halt, early close, or closed all day
};
[[nodiscard]] const char* phaseName(MarketPhase p);

// A CME metals holiday (Globex): a mid-day halt that reopens the same
// evening, an early close with no evening session, or a day closed
// outright. Times are minutes since midnight Eastern Time (ET = CT + 1h);
// reopenMinuteET is on the reopen day, reopenDayOffset days after the
// holiday date (0 = same day, 2 = Sunday for a Friday early close).
enum class HolidayKind { Halt, EarlyClose, Closed };

struct HolidayEvent {
    std::string name;
    int year = 0, month = 0, day = 0;  // New York date of the holiday
    HolidayKind kind = HolidayKind::Halt;
    int haltMinuteET = -1;             // minutes since midnight ET; -1 when Closed
    int reopenMinuteET = 18 * 60;      // 18:00 ET on the reopen day
    int reopenDayOffset = 0;           // days after the holiday date the reopen falls on
};

// The holiday whose New York date contains utcMs, if any.
[[nodiscard]] std::optional<HolidayEvent> holidayAt(int64_t utcMs);
// All CME metals holidays for `year` (>= 2026): computed from the rules
// (MLK/Presidents/Memorial/Labor Day, Thanksgiving + Black Friday,
// Juneteenth, Independence Day, Good Friday from Easter, Christmas and
// New Year observance), with the verified 2026-2027 rows overriding on
// any date conflict.
[[nodiscard]] std::vector<HolidayEvent> holidaysForYear(int year);

// True when the reopen after a halt moved more than `multiple` ATRs from
// the pre-halt close -- the market gapped; wait for spreads to settle.
// False when atr <= 0 (nothing to judge against).
[[nodiscard]] bool reopenGapTooBig(double preHaltClose, double reopenPrice, double atr, double multiple = 2.0);
// True when the spread has settled back near its median (or the median is
// unknown, <= 0, so there is nothing to judge against).
[[nodiscard]] bool spreadSettled(double spread, double medianSpread, double multiple = 1.5);

struct MarketStatus {
    MarketPhase phase = MarketPhase::Open;
    bool marketOpen = true;         // the broker quotes XAUUSD right now
    bool canOpenRange = true;       // phase == Open (or the filter is off and the market is open)
    std::string label;              // short, for a status pill: "Open", "Weekend", "Pause: London close"...
    std::string detail;             // one line of why
    std::vector<std::string> sessions;  // sessions open right now, e.g. {"London", "New York"}
    std::string sessionLabel;       // "London + New York", or "Between sessions"
    int64_t nextChangeUtcMs = 0;    // when canOpenRange flips next (0 = not within a week)
    std::string nextChangeLabel;    // "London close pause" / "Trading resumes"
    // The holiday in force now, if the market is closed for one.
    std::string holidayName;
    int64_t holidayReopenUtcMs = 0;  // when it reopens (18:00 ET on the reopen day)
    bool pauseIsHoliday = false;     // this pause is the run-up to a holiday halt/close
    // The next guarded news release (or the one being guarded now).
    bool hasNextNews = false;
    NewsEvent nextNews;
};

// Does the news guard stop trading around this event?
[[nodiscard]] bool newsIsGuarded(const NewsEvent& e, const MarketHoursConfig& config);

// Pure: the market's state at `utcMs` under `config`. nextChange* is only
// filled in when findNext is true (it scans ahead, minute by minute).
// `feed` = the ForexFactory calendar (may be null).
[[nodiscard]] MarketStatus evaluateMarket(int64_t utcMs, const MarketHoursConfig& config,
                                          const std::vector<NewsEvent>* feed = nullptr, bool findNext = true);

// Which sessions are open at utcMs, as one label -- for grouping results
// ("London + New York", "Tokyo", "Between sessions", or "Market closed").
[[nodiscard]] std::string sessionLabelAt(int64_t utcMs);

// The session's window (UTC ms) that is open at utcMs, or else the next one.
struct SessionWindow {
    int64_t openUtcMs = 0;
    int64_t closeUtcMs = 0;
    bool openNow = false;
};
[[nodiscard]] SessionWindow sessionWindow(Session s, int64_t utcMs);

// UTC offset (minutes) of a session's city at utcMs, daylight saving included.
[[nodiscard]] int sessionUtcOffsetMinutes(Session s, int64_t utcMs);
[[nodiscard]] int newYorkUtcOffsetMinutes(int64_t utcMs);
// Day of the week in New York at utcMs, 0 = Sunday .. 5 = Friday.
[[nodiscard]] int newYorkWeekday(int64_t utcMs);

// UTC ms for a UTC civil date and time.
[[nodiscard]] int64_t utcMsFromCivil(int year, int month, int day, int hour, int minute, int second);

// Milliseconds since the epoch, now.
[[nodiscard]] int64_t utcNowMs();
// "YYYY-MM-DD HH:MM" in this computer's local time <-> UTC ms. Returns
// false on a malformed string.
[[nodiscard]] bool parseLocalDateTime(const std::string& text, int64_t& utcMs);
[[nodiscard]] std::string formatLocalDateTime(int64_t utcMs);
// "YYYY-MM-DD HH:MM:SS" local (as written in the history) -> UTC ms; 0 if malformed.
[[nodiscard]] int64_t localTimestampToUtcMs(const std::string& text);

}  // namespace muuk::ctrader_fix
