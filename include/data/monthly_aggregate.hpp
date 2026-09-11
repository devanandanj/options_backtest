//
// Created by devanandan on 9-09-2026.
//

#pragma once

// #include "data/loader.hpp"
#include "data/equity_loader.hpp"
#include "strategy/strike_strategy.hpp"

#include <vector>

namespace backtester {
    std::vector<MonthlyPrice> aggregate_monthly_closes(const std::vector<EquityRow>& rows);
}