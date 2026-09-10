//
// Created by devanandan on 10-09-2026.
//

#include <gtest/gtest.h>
#include "../include/backtester/contract.hpp"

TEST(Loader, ThrowsOnMissingFile) {
    EXPECT_THROW(backtester::load_chain_csv("nonexistent.csv"), std::runtime_error);
}

TEST(Loader, ParsesRealSampleFile) {
    auto rows = backtester::load_chain_csv("data/sample/idfcfirstb_jan2024_ce_80.csv");
    ASSERT_EQ(rows.size(), 19);

    const auto& first = rows.front();
    EXPECT_EQ(first.contract.underlying, "IDFCFIRSTB");
    EXPECT_EQ(first.contract.strike, 80.0);
    EXPECT_EQ(first.contract.right, backtester::Right::Call);
    EXPECT_EQ(first.contract.lot_size, 7500);
    EXPECT_DOUBLE_EQ(first.open, 0.6);
    EXPECT_DOUBLE_EQ(first.close, 0.2);
}