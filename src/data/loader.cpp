//
// Created by devanandan on 10-09-2026.
//

#include "../../include/data/loader.hpp"
#include "../../include/backtester/contract.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace backtester {

namespace {

template <typename T>
T parse_num(std::string_view sv) {
    T val{};
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

std::chrono::year_month_day parse_date(std::string_view s) {
    int year  = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    unsigned month = (s[5] - '0') * 10 + (s[6] - '0');
    unsigned day = (s[8] - '0') * 10 + (s[9] - '0');
    return {std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
}

} // anonymous namespace

std::vector<ChainRow> load_chain_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("Cannot open file: " + path);

    std::string line;
    std::getline(file, line); // Skip header

    std::vector<ChainRow> rows;
    rows.reserve(50'000); // Pre-reserve typical session batch size

    std::string_view f[16];

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        // Slice line into string_views directly (Zero allocations)
        size_t col = 0, start = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == ',') {
                if (col < 16) f[col++] = std::string_view(line.data() + start, i - start);
                start = i + 1;
            }
        }
        if (col < 16) f[col++] = std::string_view(line.data() + start, line.size() - start);
        if (col != 16) continue;

        ChainRow& row = rows.emplace_back();
        row.date                = parse_date(f[0]);
        row.contract.expiry     = parse_date(f[1]);
        row.contract.right      = (f[2] == "CE") ? Right::Call : Right::Put;
        row.contract.strike     = parse_num<double>(f[3]);
        row.open                = parse_num<double>(f[4]);
        row.high                = parse_num<double>(f[5]);
        row.low                 = parse_num<double>(f[6]);
        row.close               = parse_num<double>(f[7]);
        row.ltp                 = parse_num<double>(f[8]);
        row.settle_price        = parse_num<double>(f[9]);
        row.total_traded_qty    = parse_num<uint64_t>(f[10]);
        row.contract.lot_size   = parse_num<int>(f[11]);
        row.premium_value       = parse_num<double>(f[12]);
        row.open_interest       = parse_num<double>(f[13]);
        row.change_in_oi        = parse_num<int64_t>(f[14]);
        row.contract.underlying.assign(f[15]);
    }

    return rows;
}

}