//
// Created by devanandan on 10-09-2026.
//
#include <gtest/gtest.h>
#include "../include/strategy/short_call.hpp"

using namespace backtester;

TEST(ShortCall, SameMonthAllSuccess) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
        {"2024-02", 105.0, 105.0},
        {"2024-03", 108.0, 108.0},
    };
    auto result = short_call_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.total_months, 3);
    EXPECT_EQ(result.successes, 3);
    EXPECT_DOUBLE_EQ(result.success_rate, 1.0);
}

TEST(ShortCall, SameMonthOneFailure) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},  // strike 110, check 100 -> success
        {"2024-02", 115.0, 115.0},  // strike 126.5, check 115 -> success
    };
    auto result = short_call_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.successes, 2);
}

TEST(ShortCall, NextMonthOffset) {
    std::vector<MonthlyPrice> prices = {
        {"2024-09", 100.0, 100.0},  // strike 110, check Oct=120 -> fail
        {"2024-10", 120.0, 120.0},  // strike 132, check Nov=125 -> success
        {"2024-11", 125.0, 125.0},  // no Dec -> excluded
    };
    auto result = short_call_strategy(prices, 0.10, 1);
    EXPECT_EQ(result.total_months, 2);   // last month has no offset target
    EXPECT_EQ(result.successes, 1);
    EXPECT_DOUBLE_EQ(result.success_rate, 0.5);
}

TEST(ShortCall, EmptyInput) {
    std::vector<MonthlyPrice> prices;
    auto result = short_call_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.total_months, 0);
    EXPECT_DOUBLE_EQ(result.success_rate, 0.0);
}

TEST(ShortCall, EntryUsesMonthStartNotMonthEnd) {
    // The month opens at 100 and rallies to 150 by month end. The strike must be
    // derived from the 100 the trader could actually see at entry (-> 110), not
    // from the 150 that is only knowable in hindsight (-> 165).
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 150.0, 100.0},
    };
    auto result = short_call_strategy(prices, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_DOUBLE_EQ(o.entry_price, 100.0);
    EXPECT_DOUBLE_EQ(o.strike, 110.0);
    EXPECT_DOUBLE_EQ(o.check_price, 150.0);  // outcome still measured at month end
    EXPECT_FALSE(o.success);                 // 150 breached the 110 strike
}

namespace {
    ChainRow make_call_row(const std::string& date, double strike, double close) {
        ChainRow row;
        row.contract.right = Right::Call;
        row.contract.strike = strike;
        row.close = close;

        int year = std::stoi(date.substr(0, 4));
        unsigned month = std::stoi(date.substr(5, 2));
        unsigned day = std::stoi(date.substr(8, 2));
        row.date = std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day};

        return row;
    }
}

TEST(ShortCall, RoundsUpToSmallestAvailableStrikeAtOrAboveTarget) {
    // entry_price 100, otm_pct 0.10 -> raw strike 110.0. OTM buffer requires
    // strike >= raw, so 108 is rejected; smallest available >= 110 is 112.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 108.0, 4.0),  // below target, must NOT be picked
        make_call_row("2024-01-01", 112.0, 2.0),  // earliest for 112
        make_call_row("2024-01-08", 112.0, 1.5),
        make_call_row("2024-01-01", 115.0, 1.0),
    };

    auto result = short_call_strategy(prices, chain, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_DOUBLE_EQ(o.strike, 110.0);
    EXPECT_DOUBLE_EQ(o.rounded_strike, 112.0);
    EXPECT_DOUBLE_EQ(o.premium, 2.0);
    EXPECT_DOUBLE_EQ(o.profit_pct, 2.0);  // 2 / 100 * 100
}

TEST(ShortCall, NeverPicksStrikeBelowTarget) {
    // entry_price 100, otm_pct 0.075 -> raw strike 107.5. 105 is nearer but
    // sits below the OTM target, so it must be rejected in favour of 110.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 105.0, 2.5),
        make_call_row("2024-01-01", 110.0, 1.0),
    };

    auto result = short_call_strategy(prices, chain, 0.075, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 110.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 1.0);
}

TEST(ShortCall, NoStrikeAboveTargetLeavesPremiumUnset) {
    // entry_price 100, otm_pct 0.10 -> raw 110. Chain only has 105 and 108
    // (both below target). Strategy should skip rather than round down into ITM.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 105.0, 5.0),
        make_call_row("2024-01-01", 108.0, 4.0),
    };

    auto result = short_call_strategy(prices, chain, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);
}

TEST(ShortCall, NoChainDataForMonthLeavesPremiumUnset) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-02-01", 110.0, 3.0),  // different month
    };

    auto result = short_call_strategy(prices, chain, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);
}