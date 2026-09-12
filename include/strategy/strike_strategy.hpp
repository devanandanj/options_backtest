//
// Created by devanandan on 10-09-2026.
//

#pragma once

#include <vector>

#include "data/loader.hpp"

namespace backtester {

    struct MonthlyPrice {
        std::string month{};
        double close{};        // close on the last trading day of the month
        double first_close{};  // close on the first trading day of the month
    };

    struct MonthOutcome {
        std::string entry_month;    // e.g. "2024-09"
        double entry_price;
        double strike;
        std::string check_month;    // e.g. "2024-10"
        double check_price;
        bool success;
        double rounded_strike{};    // strike rounded to the nearest strike present in the chain data
        double premium{};           // option close price at the start of entry_month for rounded_strike
        double profit_pct{};        // premium / entry_price * 100
    };

    struct StrategyResult {
        int total_months;
        int successes;
        double success_rate;
        std::vector<MonthOutcome> outcomes;
    };

    StrategyResult strike_strategy( const std::vector<MonthlyPrice>& prices, double strike_pct, int month_offset);

    // Also looks up, per entry month, the strike premium: the raw strike is rounded to the
    // mathematically nearest Call strike actually present in `chain` for that month, and the
    // premium is the option's close price on the earliest trading day of entry_month for that
    // rounded strike.
    StrategyResult strike_strategy( const std::vector<MonthlyPrice>& prices, const std::vector<ChainRow>& chain,
        double strike_pct, int month_offset);

    void export_output_to_csv(const StrategyResult& result, std::string_view out_dir, std::string_view filename);

    void run_strike_strategy(std::string_view csv_path, double pct, int month_offset,
        std::string_view out_dir = "out/strike_strategy");

    void run_strike_strategy(std::string_view csv_path, std::string_view chain_csv_path, double pct, int month_offset,
        std::string_view out_dir = "out/strike_strategy");

}