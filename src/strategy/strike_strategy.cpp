//
// Created by devanandan on 10-09-2026.
//

#include <../include/strategy/strike_strategy.hpp>

#include "timer.hpp"
#include "data/equity_loader.hpp"
#include <vector>
#include <filesystem>
#include <fstream>

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
            double entry_price = prices[i].close;
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

            file << "entry_month,entry_price,strike,check_month,check_price,status\n";

            for (const auto& o : result.outcomes) {
                file << o.entry_month << ','
                     << o.entry_price << ','
                     << o.strike << ','
                     << o.check_month << ','
                     << o.check_price << ','
                     << (o.success ? "SUCCESS" : "FAIL") << '\n';
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

}
