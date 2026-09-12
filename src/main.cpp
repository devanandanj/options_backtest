//
// Created by devanandan on 10-09-2026.
//

#include "../include/strategy/strike_strategy.hpp"

int main() {
    backtester::run_strike_strategy("../data/sample/idfcfirstb_underlying_2022_2026.csv",
        "../data/sample/idfcfirstb_ce_2022_2026.csv",
        0.10, 2);
    return 0;
}