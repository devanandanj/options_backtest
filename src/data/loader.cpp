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

    // Parses "YYYY-MM-DD" — NSE's date format in this feed.
    std::chrono::year_month_day parse_date(const std::string& s) {
        if (s.size() != 10) {
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

    Right parse_right(const std::string& s) {
        if (s == "CE") return Right::Call;
        if (s == "PE") return Right::Put;
        throw std::runtime_error("Unknown option type: " + s);
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
    // DATE,EXPIRY,OPTION TYPE,STRIKE PRICE,OPEN,HIGH,LOW,CLOSE,LTP,
    // SETTLE PRICE,TOTAL TRADED QUANTITY,MARKET LOT,PREMIUM VALUE,
    // OPEN INTEREST,CHANGE IN OI,SYMBOL

    std::vector<ChainRow> rows;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        auto f = split_csv_line(line);
        if (f.size() != 16) {
            throw std::runtime_error("Malformed row (expected 16 fields, got "
                + std::to_string(f.size()) + "): " + line);
        }

        ChainRow row;
        row.date                    = parse_date(f[0]);
        row.contract.expiry         = parse_date(f[1]);
        row.contract.right          = parse_right(f[2]);
        row.contract.strike         = std::stod(f[3]);
        row.open                    = std::stod(f[4]);
        row.high                    = std::stod(f[5]);
        row.low                     = std::stod(f[6]);
        row.close                   = std::stod(f[7]);
        row.ltp                     = std::stod(f[8]);
        row.settle_price            = std::stod(f[9]);
        row.total_traded_qty        = std::stol(f[10]);
        row.contract.lot_size       = std::stoi(f[11]);
        row.premium_value           = std::stod(f[12]);
        row.open_interest           = std::stod(f[13]);
        row.change_in_oi            = std::stod(f[14]);
        row.contract.underlying     = f[15];

        rows.push_back(std::move(row));
    }

    return rows;
}

}  // namespace backtester