//
// Created by devanandan on 16-09-2026.
//

#include "strategy/diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numeric>

namespace backtester {

    namespace {

        std::string date_string(const std::chrono::year_month_day& d) {
            return std::format("{:04d}-{:02d}-{:02d}", static_cast<int>(d.year()),
                static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
        }

        std::chrono::sys_days parse_date(const std::string& text) {
            return std::chrono::sys_days{
                std::chrono::year{std::stoi(text.substr(0, 4))}
                / std::chrono::month{static_cast<unsigned>(std::stoi(text.substr(5, 2)))}
                / std::chrono::day{static_cast<unsigned>(std::stoi(text.substr(8, 2)))}};
        }

        constexpr double kSessionsPerYear = 252.0;

    }

    EntryContext entry_context(const std::vector<EquityRow>& rows,
                               const std::string& entry_date,
                               const std::string& expiry_date,
                               const DiagnosticWindows& windows) {
        EntryContext out;

        // Locate the entry session. Rows arrive ascending, so a linear scan is fine
        // at this size and avoids assuming the dates form a contiguous range.
        std::size_t at = rows.size();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (date_string(rows[i].date) == entry_date) { at = i; break; }
        }

        if (!expiry_date.empty() && !entry_date.empty()) {
            out.days_to_expiry = static_cast<int>(
                (parse_date(expiry_date) - parse_date(entry_date)).count());
        }
        if (at == rows.size()) return out;   // entry day absent; complete stays false

        // Every window ends on the session BEFORE entry. Including the entry close
        // would not be lookahead, but excluding it keeps the features strictly
        // "what was known walking in", which is the whole point of measuring them.
        const std::size_t available = at;
        const int longest = std::max({windows.vol_sessions + 1, windows.momentum_sessions + 1,
                                      windows.range_sessions});
        if (available < static_cast<std::size_t>(longest)) return out;

        // Volatility: stdev of daily log returns, annualised.
        std::vector<double> returns;
        returns.reserve(static_cast<std::size_t>(windows.vol_sessions));
        for (std::size_t i = at - static_cast<std::size_t>(windows.vol_sessions); i < at; ++i) {
            const double previous = rows[i - 1].close;
            const double current = rows[i].close;
            if (previous > 0.0 && current > 0.0) returns.push_back(std::log(current / previous));
        }
        if (returns.size() > 1) {
            const double mean = std::accumulate(returns.begin(), returns.end(), 0.0)
                              / static_cast<double>(returns.size());
            double sum_squares = 0.0;
            for (const double r : returns) sum_squares += (r - mean) * (r - mean);
            const double variance = sum_squares / static_cast<double>(returns.size() - 1);
            out.trailing_vol_pct = std::sqrt(variance * kSessionsPerYear) * 100.0;
        }

        // Momentum: close-to-close over the window ending the session before entry.
        const double then = rows[at - static_cast<std::size_t>(windows.momentum_sessions)].close;
        const double now = rows[at - 1].close;
        if (then > 0.0) out.momentum_pct = (now / then - 1.0) * 100.0;

        // Range position of the entry close within the trailing high/low.
        const auto range_begin = rows.begin()
            + static_cast<std::ptrdiff_t>(at - static_cast<std::size_t>(windows.range_sessions));
        const auto range_end = rows.begin() + static_cast<std::ptrdiff_t>(at);
        double low = range_begin->close;
        double high = range_begin->close;
        for (auto it = range_begin; it != range_end; ++it) {
            low = std::min(low, it->close);
            high = std::max(high, it->close);
        }
        const double span = high - low;
        // A dead-flat window has no position to report; leaving it mid-range would
        // invent a reading the data does not support.
        if (span <= 0.0) return out;
        out.range_pos_pct = ((rows[at].close - low) / span) * 100.0;

        out.complete = true;
        return out;
    }

}
