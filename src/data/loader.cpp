//
// Created by devanandan on 10-09-2026.
//

#include "../../include/data/loader.hpp"
#include "../../include/backtester/contract.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace backtester {

namespace {
    std::vector<std::string> split_csv_line(const std::string& line) {
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }
        return fields;
    }

    std::chrono::year_month_day parse_date(const std::string& s) {
        if (s.size() < 10) {
            throw std::runtime_error("Unexpected date format: " + s);
        }
        int year  = std::stoi(s.substr(0, 4));
        unsigned month = std::stoi(s.substr(5, 2));
        unsigned day   = std::stoi(s.substr(8, 2));
        return std::chrono::year_month_day{
            std::chrono::year{year},
            std::chrono::month{month},
            std::chrono::day{day}
        };
    }
}

std::vector<ChainRow> load_chain_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    std::string header_line;
    std::getline(file, header_line);
    // Expected header:
    // DATE,EXPIRY,OPTION TYPE,STRIKE PRICE,OPEN,HIGH,LOW,CLOSE,LTP,SETTLE PRICE,
    // TOTAL TRADED QUANTITY,MARKET LOT,PREMIUM VALUE,OPEN INTEREST,CHANGE IN OI,SYMBOL

    std::vector<ChainRow> rows;
    std::string line;

    // Some NSE responses include stub rows for strikes that were listed but never
    // traded and have no settle price — every numeric field is blank. These carry
    // no information the strategy can use, so we skip them at load time. For the
    // remaining rows a few fields (typically OI and change-in-OI on quiet strikes)
    // can still be blank; treat those as zero rather than failing the whole load.
    auto to_double = [](const std::string& s) { return s.empty() ? 0.0 : std::stod(s); };
    auto to_ull    = [](const std::string& s) -> uint64_t { return s.empty() ? 0ULL : std::stoull(s); };
    auto to_int    = [](const std::string& s) { return s.empty() ? 0 : std::stoi(s); };
    auto to_i64    = [](const std::string& s) -> int64_t { return s.empty() ? int64_t{0} : std::stoll(s); };

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        auto f = split_csv_line(line);
        if (f.size() != 16) {
            throw std::runtime_error("Malformed row (expected 16 fields, got "
                + std::to_string(f.size()) + "): " + line);
        }

        // No CLOSE → no premium the strategy can use.
        if (f[7].empty()) continue;

        ChainRow row;
        row.date                 = parse_date(f[0]);
        row.contract.expiry      = parse_date(f[1]);
        row.contract.right       = (f[2] == "CE") ? Right::Call : Right::Put;
        row.contract.strike      = to_double(f[3]);
        row.open                 = to_double(f[4]);
        row.high                 = to_double(f[5]);
        row.low                  = to_double(f[6]);
        row.close                = to_double(f[7]);
        row.ltp                  = to_double(f[8]);
        row.settle_price         = to_double(f[9]);
        row.total_traded_qty     = to_ull(f[10]);
        row.contract.lot_size    = to_int(f[11]);
        row.premium_value        = to_double(f[12]);
        row.open_interest        = to_double(f[13]);
        row.change_in_oi         = to_i64(f[14]);
        row.contract.underlying  = f[15];

        rows.push_back(std::move(row));
    }

    return rows;
}

}  // namespace backtester