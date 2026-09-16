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

    // True when the file carries index levels (SERIES=INDEX) rather than an equity
    // series. An index cannot be owned, so the share leg does not apply to it - see
    // PositionConfig::cash_settled.
    bool is_index_series(const std::string& path);

}