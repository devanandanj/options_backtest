//
// Created by devanandan on 10-09-2026.
//

#include "../include/strategy/short_call.hpp"

int main() {
    backtester::run_short_call("../data/sample/idfcfirstb_underlying_2022_2026.csv",
        "../data/sample/idfcfirstb_ce_2022_2026.csv",
        0.10, 2);
    return 0;
}