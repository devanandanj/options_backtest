//
// Created by devanandan on 10-09-2026.
//

#include "data/monthly_aggregate.hpp"
#include <map>
#include <format>

namespace backtester {

    std::vector<MonthlyPrice> aggregate_monthly_closes(const std::vector<EquityRow>& rows) {
        std::map<int, EquityRow> latest_per_month;

        for (const auto& row : rows) {
            // If EquityRow stores the series column, skip bond series:
            // if (row.series != "EQ") continue;

            int key = static_cast<int>(row.date.year()) * 100
                    + static_cast<unsigned>(row.date.month());

            auto it = latest_per_month.find(key);
            if (it == latest_per_month.end() || row.date > it->second.date) {
                latest_per_month[key] = row;
            }
        }

        std::vector<MonthlyPrice> result;
        result.reserve(latest_per_month.size());

        for (const auto& [key, row] : latest_per_month) {
            int year = key / 100;
            unsigned month = key % 100;
            result.push_back(MonthlyPrice{
                std::format("{:04d}-{:02d}", year, month),
                row.close
            });
        }

        return result;
    }

}