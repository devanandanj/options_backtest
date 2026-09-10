//
// Created by devanandan on 10-09-2026.
//

#pragma once

#include <string>
#include <chrono>

namespace backtester {
    enum class Right { Call, Put};

    struct Contract {
        std::string underlying;
        double strike;
        Right right;
        std::chrono::year_month_day expiry;
        int lot_size;
        bool operator==(const Contract&) const = default;
    };
}