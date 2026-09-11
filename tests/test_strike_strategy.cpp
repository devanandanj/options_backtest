//
// Created by devanandan on 10-09-2026.
//
#include <gtest/gtest.h>
#include "../include/strategy/strike_strategy.hpp"

using namespace backtester;

TEST(StrikeStrategy, SameMonthAllSuccess) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0},
        {"2024-02", 105.0},
        {"2024-03", 108.0},
    };
    auto result = strike_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.total_months, 3);
    EXPECT_EQ(result.successes, 3);
    EXPECT_DOUBLE_EQ(result.success_rate, 1.0);
}

TEST(StrikeStrategy, SameMonthOneFailure) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0},  // strike 110, check 100 -> success
        {"2024-02", 115.0},  // strike 126.5, check 115 -> success
    };
    auto result = strike_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.successes, 2);
}

TEST(StrikeStrategy, NextMonthOffset) {
    std::vector<MonthlyPrice> prices = {
        {"2024-09", 100.0},  // strike 110, check Oct=120 -> fail
        {"2024-10", 120.0},  // strike 132, check Nov=125 -> success
        {"2024-11", 125.0},  // no Dec -> excluded
    };
    auto result = strike_strategy(prices, 0.10, 1);
    EXPECT_EQ(result.total_months, 2);   // last month has no offset target
    EXPECT_EQ(result.successes, 1);
    EXPECT_DOUBLE_EQ(result.success_rate, 0.5);
}

TEST(StrikeStrategy, EmptyInput) {
    std::vector<MonthlyPrice> prices;
    auto result = strike_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.total_months, 0);
    EXPECT_DOUBLE_EQ(result.success_rate, 0.0);
}