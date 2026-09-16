//
// Created by devanandan on 12-09-2026.
//

#include "strategy/position.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <unordered_map>

namespace backtester {

    namespace {

        std::string month_of_date(const std::chrono::year_month_day& date) {
            return std::format("{:04d}-{:02d}", static_cast<int>(date.year()),
                static_cast<unsigned>(date.month()));
        }

        // "YYYY-MM" shifted by whole months.
        std::string shift_month(const std::string& month, int delta) {
            const int ordinal = std::stoi(month.substr(0, 4)) * 12
                              + std::stoi(month.substr(5, 2)) - 1 + delta;
            return std::format("{:04d}-{:02d}", ordinal / 12, ordinal % 12 + 1);
        }

    }

    LotSizes build_lot_sizes(const std::vector<ChainRow>& chain) {
        // Counted rather than taken from the first row seen: a revision lands
        // mid-month, and the lot the month mostly traded on is the representative one.
        std::unordered_map<std::string, std::map<std::uint64_t, int>> tally;
        for (const auto& row : chain) {
            if (row.contract.lot_size <= 0) continue;
            tally[month_of_date(row.date)][static_cast<std::uint64_t>(row.contract.lot_size)]++;
        }

        LotSizes lots;
        lots.reserve(tally.size());
        for (const auto& [month, counts] : tally) {
            const auto best = std::max_element(counts.begin(), counts.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            lots[month] = LotSize{best->first, true};
        }
        return lots;
    }

    PositionResult simulate_covered_call(const StrategyResult& result,
                                         const LotSizes& lots,
                                         const PositionConfig& config) {
        PositionResult out;

        if (config.cash_settled) {
            out.not_applicable = "cash-settled underlying: no shares to hold or be "
                                 "assigned, so there is no covered-call position and "
                                 "no buy-and-hold benchmark to compare against";
            return out;
        }

        // .second is false when the number came from configuration rather than NSE.
        const auto lot_for = [&](const std::string& month) -> std::pair<std::uint64_t, bool> {
            const auto it = lots.find(month);
            if (it != lots.end() && it->second.shares > 0) return {it->second.shares, true};
            return {config.lot_size_override, false};   // 0 means "still unknown"
        };

        double cash = 0.0;
        std::uint64_t shares = 0;
        std::uint64_t held_shares = 0;   // the fixed size of the holding
        bool started = false;
        std::string next_eligible_month;   // empty until the first cycle is taken

        for (const auto& outcome : result.outcomes) {
            // `entered`, not `priced`: a cycle the yield filter turned away had a
            // perfectly good contract behind it, but no position was ever opened.
            if (!outcome.entered) continue;
            if (!next_eligible_month.empty() && outcome.entry_month < next_eligible_month) {
                continue;   // a cycle is still open; one lot covers one call
            }

            const auto [lot, lot_from_chain] = lot_for(outcome.entry_month);
            if (lot == 0) { ++out.skipped_unknown_lot; continue; }

            if (!started) {
                // The holding is sized once, here, and then held. Buying at this
                // cycle's entry puts the benchmark on the same shares from the same day.
                held_shares = lot * config.lots;
                shares = held_shares;
                cash = -static_cast<double>(shares) * outcome.entry_price;
                started = true;
            } else if (shares < held_shares) {
                if (!config.rebuy_after_assignment) break;   // parked in cash by choice
                // Re-entry is this month's first close, not the assignment price: the
                // gap between being called away and buying back is time spent in cash,
                // and the stock moves across it.
                const std::uint64_t buy = held_shares - shares;
                cash -= static_cast<double>(buy) * outcome.entry_price;
                shares = held_shares;
            }

            // A revised lot changes how many calls the holding covers, not the holding.
            const std::uint64_t contracts = shares / lot;
            if (contracts == 0) {           // the lot grew past the whole holding
                ++out.skipped_under_one_lot;
                continue;
            }
            const std::uint64_t covered = contracts * lot;

            Cycle cycle;
            cycle.entry_month = outcome.entry_month;
            cycle.check_month = outcome.check_month;
            cycle.shares = shares;
            cycle.lot_size = lot;
            if (!lot_from_chain) ++out.cycles_on_override;
            cycle.contracts = contracts;
            cycle.covered_shares = covered;
            cycle.entry_spot = outcome.entry_price;
            cycle.strike = outcome.rounded_strike;
            cycle.exit_spot = outcome.check_price;

            cycle.premium_collected = outcome.premium * static_cast<double>(covered);
            cash += cycle.premium_collected;

            // The verdict the option leg already reached, now applied to the shares.
            // Only the covered portion is called away.
            cycle.assigned = !outcome.held;
            if (cycle.assigned) {
                cycle.assignment_proceeds = outcome.rounded_strike * static_cast<double>(covered);
                cash += cycle.assignment_proceeds;
                shares -= covered;
                ++out.assignments;
            }

            cycle.cash = cash;
            cycle.shares_value = static_cast<double>(shares) * outcome.check_price;
            cycle.pnl = cash + cycle.shares_value;
            out.cycles.push_back(cycle);

            next_eligible_month = shift_month(outcome.check_month, 1);
        }

        if (!out.cycles.empty()) {
            const auto& last = out.cycles.back();
            out.end_pnl = last.pnl;
            out.shares_held_at_end = shares;

            // Benchmark: the same shares bought on the same day, marked at the same
            // final price the strategy is marked at.
            const std::uint64_t bench_shares = held_shares;
            const double bought = static_cast<double>(bench_shares) * out.cycles.front().entry_spot;
            out.benchmark_pnl =
                static_cast<double>(bench_shares) * last.exit_spot - bought;
            out.excess_pnl = out.end_pnl - out.benchmark_pnl;
        }
        return out;
    }

}
