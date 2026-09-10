//
// Created by devanandan on 10-09-2026.
//

#include <vector>
#include <iostream>

#include "../include/backtester/contract.hpp"
#include "../include/data/loader.hpp"

int main() {
    auto rows = backtester::load_chain_csv("../data/sample/idfcfirstb_jan2024_ce_80.csv");
    std::cout << "Loaded " << rows.size() << " rows" << std::endl;
    return 0;
}
