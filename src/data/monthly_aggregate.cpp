//
// Created by devanandan on 10-09-2026.
//

#include "data/monthly_aggregate.hpp"
#include <map>
#include <format>

namespace backtester {

    std::vector<MonthlyPrice> aggregate_monthly_closes(const std::vector<EquityRow>& rows) {
        // Both ends of the month are needed: the first trading day's close is the
        // entry reference (picking a strike from the month-end close would be
        // lookahead), the last trading day's close is the outcome reference.
        struct MonthBounds {
            EquityRow first{};
            EquityRow last{};
            bool seen{false};
        };
        std::map<int, MonthBounds> per_month;

        for (const auto& row : rows) {
            int key = static_cast<int>(row.date.year()) * 100
                    + static_cast<unsigned>(row.date.month());

            auto& b = per_month[key];
            if (!b.seen) {
                b.first = row;
                b.last = row;
                b.seen = true;
                continue;
            }
            if (row.date < b.first.date) b.first = row;
            if (row.date > b.last.date)  b.last = row;
        }

        std::vector<MonthlyPrice> result;
        result.reserve(per_month.size());

        for (const auto& [key, b] : per_month) {
            int year = key / 100;
            unsigned month = key % 100;
            result.push_back(MonthlyPrice{
                std::format("{:04d}-{:02d}", year, month),
                b.last.close,
                b.first.close
            });
        }

        return result;
    }

}