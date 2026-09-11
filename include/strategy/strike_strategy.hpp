//
// Created by devanandan on 10-09-2026.
//

#pragma once

#include <vector>

namespace backtester {

    struct MonthlyPrice {
        std::string month{};
        double close{};
    };

    struct MonthOutcome {
        std::string entry_month;    // e.g. "2024-09"
        double entry_price;
        double strike;
        std::string check_month;    // e.g. "2024-10"
        double check_price;
        bool success;
    };

    struct StrategyResult {
        int total_months;
        int successes;
        double success_rate;
        std::vector<MonthOutcome> outcomes;
    };

    StrategyResult strike_strategy( const std::vector<MonthlyPrice>& prices, double strike_pct, int month_offset);

    void export_output_to_csv(const StrategyResult& result, std::string_view out_dir, std::string_view filename);

    void run_strike_strategy(std::string_view csv_path, double pct, int month_offset,
        std::string_view out_dir = "out/strike_strategy");

}