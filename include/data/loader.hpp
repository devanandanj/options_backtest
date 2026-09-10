//
// Created by devanandan on 10-09-2026.
//
#pragma once
#include "../backtester/contract.hpp"

#include <vector>
#include <string>

namespace backtester {
    struct ChainRow {
        Contract contract;
        std::chrono::year_month_day date{};
        double open{}, high{}, low{}, close{}, ltp{}, settle_price{};
        long total_traded_qty{};
        double premium_value{};
        double open_interest{};
        long change_in_oi{};
    };

    std::vector<ChainRow> load_chain_csv(const std::string& path);
}
