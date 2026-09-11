//
// Created by devanandan on 10-09-2026.
//

#pragma once

#include <vector>
#include <string>
#include <chrono>

namespace backtester {

    struct EquityRow {
        std::chrono::year_month_day date{};
        double close{};
    };

    std::vector<EquityRow> load_equity_csv(const std::string& path);

}