//
// Created by devanandan on 10-09-2026.
//

#include "data/equity_loader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>

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

// Expects "DD-Mon-YYYY" (jugaad_data's stock_df date format), e.g. "05-Jan-2024"
    std::chrono::year_month_day parse_date(const std::string& s) {
    int year  = std::stoi(s.substr(0, 4));
    int month = std::stoi(s.substr(5, 2));
    int day   = std::stoi(s.substr(8, 2));

    return std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)}
    / std::chrono::day{static_cast<unsigned>(day)};
}

} // anonymous namespace

    bool is_index_series(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::string line;
        std::getline(file, line);          // header
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            const auto f = split_csv_line(line);
            if (f.size() != 15) continue;
            return f[1] == "INDEX";        // the first data row settles it
        }
        return false;
    }

    std::vector<EquityRow> load_equity_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    std::string header_line;
    std::getline(file, header_line);

    std::vector<EquityRow> rows;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        auto f = split_csv_line(line);
        if (f.size() != 15) continue;

        // Skip bond tranches (NC, NB, ND, NE) and block deals (BL). NSE files the
        // same symbol under several series, and an NCD tranche trades at par value
        // rather than the share price - letting one through would put 1,100 beside
        // 49.65 in the same series.
        //
        // INDEX is accepted alongside EQ because an index has no series of its own:
        // tools/prepare_index.py writes the level series in this schema and marks it
        // INDEX rather than mislabelling an index as an equity.
        if (f[1] != "EQ" && f[1] != "INDEX") continue;

        EquityRow row;
        row.date  = parse_date(f[0]);
        row.close = std::stod(f[7]);
        // map other fields if your struct has them

        rows.push_back(row);
    }

    return rows;
}

}