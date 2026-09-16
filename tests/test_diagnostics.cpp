//
// Entry-time market context. Every assertion here is really about one property:
// the features describe what was knowable walking into the trade, and nothing else.
//

#include <gtest/gtest.h>
#include <cmath>
#include "../include/strategy/diagnostics.hpp"

using namespace backtester;

namespace {

    std::chrono::year_month_day ymd(const std::string& date) {
        return std::chrono::year{std::stoi(date.substr(0, 4))}
             / std::chrono::month{static_cast<unsigned>(std::stoi(date.substr(5, 2)))}
             / std::chrono::day{static_cast<unsigned>(std::stoi(date.substr(8, 2)))};
    }

    // A run of consecutive days starting 2024-01-01, one close each. Calendar
    // weekends are irrelevant here: the windows count sessions, not dates.
    std::vector<EquityRow> series(const std::vector<double>& closes) {
        std::vector<EquityRow> rows;
        int day = 1, month = 1, year = 2024;
        for (double c : closes) {
            EquityRow r;
            r.close = c;
            r.date = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)}
                   / std::chrono::day{static_cast<unsigned>(day)};
            rows.push_back(r);
            if (++day > 28) { day = 1; ++month; }
            if (month > 12) { month = 1; ++year; }
        }
        return rows;
    }

    std::string date_at(const std::vector<EquityRow>& rows, std::size_t i) {
        const auto& d = rows[i].date;
        char buf[11];
        std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", static_cast<int>(d.year()),
                      static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
        return std::string(buf);
    }

    DiagnosticWindows shortWindows() {
        DiagnosticWindows w;
        w.vol_sessions = 5;
        w.momentum_sessions = 5;
        w.range_sessions = 10;
        return w;
    }

}

TEST(Diagnostics, IsIncompleteWhenHistoryIsShorterThanTheWindows) {
    // Early cycles have nothing behind them. Reporting a range position computed
    // from four days beside one computed from a full year would put two different
    // measurements in the same column.
    auto rows = series({100, 101, 102, 103});
    const auto ctx = entry_context(rows, date_at(rows, 3), "", shortWindows());
    EXPECT_FALSE(ctx.complete);
}

TEST(Diagnostics, IgnoresEverythingAfterTheEntryDay) {
    // The tail of this series collapses. If any window reached past entry, the
    // features would carry knowledge of a crash that had not happened yet -
    // which would make every finding about losing months a tautology.
    // A gently varying run-in, so the trailing range has real width, followed by
    // an entry session the two series share. Only the sessions AFTER it differ.
    std::vector<double> shared;
    for (int i = 0; i < 20; ++i) shared.push_back(100.0 + (i % 4));
    shared.push_back(101.0);   // the entry session

    std::vector<double> calm = shared;
    calm.insert(calm.end(), {101.0, 101.0});

    std::vector<double> crash = shared;
    crash.insert(crash.end(), {40.0, 10.0});

    auto calm_rows = series(calm);
    auto crash_rows = series(crash);
    const std::size_t entry = 20;

    const auto a = entry_context(calm_rows, date_at(calm_rows, entry), "", shortWindows());
    const auto b = entry_context(crash_rows, date_at(crash_rows, entry), "", shortWindows());

    ASSERT_TRUE(a.complete);
    ASSERT_TRUE(b.complete);
    EXPECT_DOUBLE_EQ(a.trailing_vol_pct, b.trailing_vol_pct);
    EXPECT_DOUBLE_EQ(a.momentum_pct, b.momentum_pct);
    EXPECT_DOUBLE_EQ(a.range_pos_pct, b.range_pos_pct);
}

TEST(Diagnostics, MomentumIsTheRiseIntoTheTrade) {
    // Flat at 100, then a climb to 110 over the five sessions before entry.
    std::vector<double> closes(20, 100.0);
    for (double c : {102.0, 104.0, 106.0, 108.0, 110.0}) closes.push_back(c);
    closes.push_back(112.0);   // the entry session itself

    auto rows = series(closes);
    const auto ctx = entry_context(rows, date_at(rows, closes.size() - 1), "", shortWindows());

    ASSERT_TRUE(ctx.complete);
    // From the close five sessions back (102) to the session before entry (110).
    EXPECT_NEAR(ctx.momentum_pct, (110.0 / 102.0 - 1.0) * 100.0, 1e-9);
}

TEST(Diagnostics, RangePositionIsHighAtTheTopOfTheRange) {
    std::vector<double> closes;
    for (int i = 0; i < 20; ++i) closes.push_back(90.0 + i);   // 90..109
    closes.push_back(109.0);                                   // entry at the top

    auto rows = series(closes);
    const auto ctx = entry_context(rows, date_at(rows, closes.size() - 1), "", shortWindows());

    ASSERT_TRUE(ctx.complete);
    EXPECT_GT(ctx.range_pos_pct, 90.0);
}

TEST(Diagnostics, RangePositionIsLowAtTheBottomOfTheRange) {
    std::vector<double> closes;
    for (int i = 0; i < 20; ++i) closes.push_back(109.0 - i);  // 109..90
    closes.push_back(90.0);                                    // entry at the bottom

    auto rows = series(closes);
    const auto ctx = entry_context(rows, date_at(rows, closes.size() - 1), "", shortWindows());

    ASSERT_TRUE(ctx.complete);
    EXPECT_LT(ctx.range_pos_pct, 10.0);
}

TEST(Diagnostics, AFlatWindowHasNoRangePositionToReport) {
    // Every close identical: there is no position within a range of zero width,
    // and calling it the midpoint would invent a reading.
    std::vector<double> closes(25, 100.0);
    auto rows = series(closes);
    const auto ctx = entry_context(rows, date_at(rows, 24), "", shortWindows());
    EXPECT_FALSE(ctx.complete);
}

TEST(Diagnostics, VolatilityRisesWithADisturbedRunIn) {
    std::vector<double> calm(20, 100.0);
    calm.push_back(100.0);

    std::vector<double> choppy(16, 100.0);
    for (double c : {90.0, 110.0, 88.0, 112.0, 100.0}) choppy.push_back(c);

    auto calm_rows = series(calm);
    auto choppy_rows = series(choppy);

    const auto a = entry_context(calm_rows, date_at(calm_rows, 20), "", shortWindows());
    const auto b = entry_context(choppy_rows, date_at(choppy_rows, 20), "", shortWindows());

    ASSERT_TRUE(b.complete);
    EXPECT_GT(b.trailing_vol_pct, a.trailing_vol_pct);
    EXPECT_GT(b.trailing_vol_pct, 0.0);
}

TEST(Diagnostics, DaysToExpiryIsCalendarDistanceAndSurvivesMissingHistory) {
    // Reported even when the windows cannot be filled: it needs no history, and a
    // cycle entered close to expiry is worth seeing regardless.
    auto rows = series({100, 101});
    const auto ctx = entry_context(rows, "2024-01-02", "2024-01-25", shortWindows());
    EXPECT_FALSE(ctx.complete);
    EXPECT_EQ(ctx.days_to_expiry, 23);
}

TEST(Diagnostics, AnEntryDayAbsentFromHistoryYieldsNothing) {
    auto rows = series({100, 101, 102});
    const auto ctx = entry_context(rows, "2030-06-01", "", shortWindows());
    EXPECT_FALSE(ctx.complete);
}
