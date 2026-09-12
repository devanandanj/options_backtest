//
// Created by devanandan on 10-09-2026.
//

#include <../include/strategy/strike_strategy.hpp>

#include "timer.hpp"
#include "data/equity_loader.hpp"
#include <vector>
#include <filesystem>
#include <fstream>
#include <format>
#include <algorithm>
#include <limits>

#include "data/monthly_aggregate.hpp"

namespace backtester{
    StrategyResult strike_strategy(
        const std::vector<MonthlyPrice>& prices,
        double strike_pct,
        int month_offset
    ) {
        int total = 0;
        int successes = 0;
        std::vector<MonthOutcome> outcomes;

        for (size_t i = 0; i + month_offset < prices.size(); ++i) {
            // Entry is the first trading day of the month, matching where the
            // premium is read from. Using the month-end close here would pick a
            // strike with information the trader could not have had at entry.
            double entry_price = prices[i].first_close;
            double strike = entry_price * (1.0 + strike_pct);
            double check_price = prices[i + month_offset].close;
            bool success = check_price < strike;

            outcomes.push_back(MonthOutcome{
                prices[i].month, entry_price, strike,
                prices[i + month_offset].month, check_price, success
            });

            if (success) ++successes;
            ++total;
        }

        double rate = (total > 0) ? static_cast<double>(successes) / total : 0.0;
        return StrategyResult{total, successes, rate, outcomes};
    }

    namespace {
        std::string month_of(const std::chrono::year_month_day& date) {
            return std::format("{:04d}-{:02d}", static_cast<int>(date.year()),
                static_cast<unsigned>(date.month()));
        }

        // Smallest Call strike in `chain` for `month` that is >= raw_strike.
        // OTM-buffer strategies require the traded strike to be at least the target;
        // nearest-rounding is unsafe because it can pick a strike below entry_price,
        // turning an "OTM call" into an ITM one. Returns false if no available
        // strike sits at or above the raw target for that month.
        bool round_up_to_available_strike(const std::vector<ChainRow>& chain, const std::string& month,
            double raw_strike, double& out_strike) {

            bool found = false;
            double best = std::numeric_limits<double>::infinity();

            for (const auto& row : chain) {
                if (row.contract.right != Right::Call) continue;
                if (month_of(row.date) != month) continue;

                double s = row.contract.strike;
                if (s + 1e-9 < raw_strike) continue;  // must be >= raw target
                if (s < best) {
                    best = s;
                    found = true;
                }
            }

            if (!found) return false;
            out_strike = best;
            return true;
        }

        // Option close price on the earliest trading day of `month` for the given Call strike.
        bool premium_at_month_start(const std::vector<ChainRow>& chain, const std::string& month,
            double strike, double& out_premium) {

            const ChainRow* earliest = nullptr;
            for (const auto& row : chain) {
                if (row.contract.right != Right::Call) continue;
                if (row.contract.strike != strike) continue;
                if (month_of(row.date) != month) continue;

                if (!earliest || row.date < earliest->date) earliest = &row;
            }

            if (!earliest) return false;
            out_premium = earliest->close;
            return true;
        }
    }

    StrategyResult strike_strategy(
        const std::vector<MonthlyPrice>& prices,
        const std::vector<ChainRow>& chain,
        double strike_pct,
        int month_offset
    ) {
        StrategyResult result = strike_strategy(prices, strike_pct, month_offset);

        for (auto& o : result.outcomes) {
            double rounded_strike{};
            if (!round_up_to_available_strike(chain, o.entry_month, o.strike, rounded_strike)) continue;

            double premium{};
            if (!premium_at_month_start(chain, o.entry_month, rounded_strike, premium)) continue;

            o.rounded_strike = rounded_strike;
            o.premium = premium;

            // The trade is against the exchange-listed rounded strike, so SUCCESS
            // and payoff must use the same strike or they can disagree in sign
            // (e.g. rounded < raw and rounded < check < raw is a "raw-strike win"
            // but a rounded-strike ITM breach).
            o.success = o.check_price < rounded_strike;

            double intrinsic_loss = std::max(0.0, o.check_price - rounded_strike);
            double payoff = premium - intrinsic_loss;
            o.profit_pct = (o.entry_price != 0.0) ? (payoff / o.entry_price) * 100.0 : 0.0;
        }

        // Recount success rate after the per-outcome success flags were updated.
        result.successes = 0;
        for (const auto& o : result.outcomes) {
            if (o.success) ++result.successes;
        }
        result.success_rate = (result.total_months > 0)
            ? static_cast<double>(result.successes) / result.total_months
            : 0.0;

        return result;
    }

    void export_output_to_csv(const StrategyResult& result, std::string_view out_dir, std::string_view filename) {
        namespace fs = std::filesystem;

        try {
            fs::path dir_path{std::string(out_dir)};

            std::error_code ec;
            fs::create_directories(dir_path, ec);
            if (ec) {
                std::cerr << "[Export Error] Failed to create directories: " << ec.message() << '\n';
                return;
            }

            fs::path file_path = dir_path / std::string(filename);
            std::ofstream file(file_path);

            if (!file.is_open()) {
                std::cerr << "[Export Error] Could not open file for writing: "
                          << fs::absolute(file_path).string() << '\n';
                return;
            }

            file << "entry_month,entry_price,strike,check_month,check_price,status,"
                    "rounded_strike,premium,profit_pct\n";

            for (const auto& o : result.outcomes) {
                file << o.entry_month << ','
                     << o.entry_price << ','
                     << o.strike << ','
                     << o.check_month << ','
                     << o.check_price << ','
                     << (o.success ? "SUCCESS" : "FAIL") << ','
                     << o.rounded_strike << ','
                     << o.premium << ','
                     << o.profit_pct << '\n';
            }

            file.flush();
            file.close();

            std::cout << "[CSV Export] Written " << result.outcomes.size()
                      << " records to: " << fs::absolute(file_path).string() << "\n";

        } catch (const std::exception& e) {
            std::cerr << "[Export Exception] " << e.what() << '\n';
        }
    }

    void run_strike_strategy(std::string_view csv_path, double pct, int month_offset, std::string_view out_dir) {
        const auto rows = [&] {
            Timer t{"Loading CSV"};
            return load_equity_csv(std::string(csv_path));
        }();

        std::cout << "Loaded " << rows.size() << " rows." << std::endl;

        const auto monthly = aggregate_monthly_closes(rows);
        std::cout << "Aggregated into " << monthly.size() << " months\n";

        const auto result = strike_strategy(monthly, pct, month_offset);

        for (const auto& o : result.outcomes) {
            std::cout << o.entry_month << " (entry " << o.entry_price
                      << ", strike " << o.strike << ") -> "
                      << o.check_month << " close " << o.check_price
                      << " -> " << (o.success ? "SUCCESS" : "FAIL") << "\n";
        }

        std::cout << (pct * 100.0) << "% strike (" << month_offset << "-month out) -> success rate: "
                  << result.success_rate << " ("
                  << result.successes << "/" << result.total_months << ")\n";

        // Derive dynamic output filename from input path and parameters
        namespace fs = std::filesystem;
        const std::string stem = fs::path(std::string(csv_path)).stem().string();

        const long pct_int = std::lround(pct * 100.0);

        const std::string out_filename = stem
            + "_strike_buffer_pct" + std::to_string(pct_int)
            + "_monthly_offset" + std::to_string(month_offset)
            + ".csv";

        export_output_to_csv(result, out_dir, out_filename);
    }

    void run_strike_strategy(std::string_view csv_path, std::string_view chain_csv_path, double pct,
        int month_offset, std::string_view out_dir) {

        const auto rows = [&] {
            Timer t{"Loading CSV"};
            return load_equity_csv(std::string(csv_path));
        }();

        std::cout << "Loaded " << rows.size() << " rows." << std::endl;

        const auto chain = [&] {
            Timer t{"Loading chain CSV"};
            return load_chain_csv(std::string(chain_csv_path));
        }();

        std::cout << "Loaded " << chain.size() << " chain rows." << std::endl;

        const auto monthly = aggregate_monthly_closes(rows);
        std::cout << "Aggregated into " << monthly.size() << " months\n";

        const auto result = strike_strategy(monthly, chain, pct, month_offset);

        for (const auto& o : result.outcomes) {
            std::cout << o.entry_month << " (entry " << o.entry_price
                      << ", strike " << o.strike << " -> rounded " << o.rounded_strike
                      << ", premium " << o.premium << ", profit " << o.profit_pct << "%) -> "
                      << o.check_month << " close " << o.check_price
                      << " -> " << (o.success ? "SUCCESS" : "FAIL") << "\n";
        }

        std::cout << (pct * 100.0) << "% strike (" << month_offset << "-month out) -> success rate: "
                  << result.success_rate << " ("
                  << result.successes << "/" << result.total_months << ")\n";

        namespace fs = std::filesystem;
        const std::string stem = fs::path(std::string(csv_path)).stem().string();

        const long pct_int = std::lround(pct * 100.0);

        const std::string out_filename = stem
            + "_strike_buffer_pct" + std::to_string(pct_int)
            + "_monthly_offset" + std::to_string(month_offset)
            + ".csv";

        export_output_to_csv(result, out_dir, out_filename);
    }

}
