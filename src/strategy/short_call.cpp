//
// Created by devanandan on 10-09-2026.
//

#include <../include/strategy/short_call.hpp>

#include "timer.hpp"
#include "data/equity_loader.hpp"
#include <vector>
#include <filesystem>
#include <fstream>
#include <format>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

#include "data/monthly_aggregate.hpp"

namespace backtester{

    namespace {
        // Calendar distance in months between two "YYYY-MM" labels.
        int months_between(const std::string& from, const std::string& to) {
            const auto ordinal = [](const std::string& m) {
                return std::stoi(m.substr(0, 4)) * 12 + std::stoi(m.substr(5, 2));
            };
            return ordinal(to) - ordinal(from);
        }
    }

    StrategyResult short_call_strategy(
        const std::vector<MonthlyPrice>& prices,
        double otm_pct,
        int month_offset
    ) {
        int total = 0;
        int held_months = 0;
        std::vector<MonthOutcome> outcomes;

        for (size_t i = 0; i + month_offset < prices.size(); ++i) {
            // month_offset counts calendar months, but prices is indexed by position,
            // and the two only agree while the series has no gaps. A month absent from
            // the data would otherwise stretch the hold without saying so: with May
            // missing, an April entry at offset 1 gets checked against June. Skip
            // rather than report a trade under a holding period it did not have.
            if (months_between(prices[i].month, prices[i + month_offset].month) != month_offset) {
                continue;
            }

            // Entry is the first trading day of the month, matching where the
            // premium is read from. Using the month-end close here would pick a
            // strike with information the trader could not have had at entry.
            double entry_price = prices[i].first_close;
            double strike = entry_price * (1.0 + otm_pct);
            double check_price = prices[i + month_offset].close;
            bool held = check_price < strike;

            outcomes.push_back(MonthOutcome{
                prices[i].month, entry_price, strike,
                prices[i + month_offset].month, check_price, held
            });

            if (held) ++held_months;
            ++total;
        }

        double rate = (total > 0) ? static_cast<double>(held_months) / total : 0.0;
        return StrategyResult{total, held_months, rate, outcomes};
    }

    namespace {
        std::string month_of(const std::chrono::year_month_day& date) {
            return std::format("{:04d}-{:02d}", static_cast<int>(date.year()),
                static_cast<unsigned>(date.month()));
        }

        // Smallest quote for `month` whose strike is >= raw_strike AND that was quoted
        // on entry_date.
        //
        // OTM-buffer strategies require the traded strike to be at least the target;
        // nearest-rounding is unsafe because it can pick a strike below entry_price,
        // turning an "OTM call" into an ITM one. The entry_date match is what keeps the
        // trade coherent: premium and strike must be struck on the same day, or a
        // contract first listed late in the month gets entered at a price set weeks
        // after the strike was chosen from day-one spot.
        //
        // Scans upward from the first qualifying strike because a strike listed later
        // in the month must be skipped rather than aborting the month outright — the
        // next strike up may well have been quoted on day one.
        const StrikeQuote* quote_at_or_above(const ChainIndex& index, const std::string& key,
            double raw_strike, const std::string& entry_date,
            std::uint64_t min_entry_volume) {

            auto it = index.find(key);
            if (it == index.end()) return nullptr;

            const auto& quotes = it->second;  // sorted ascending by strike
            auto hit = std::lower_bound(quotes.begin(), quotes.end(), raw_strike - 1e-9,
                [](const StrikeQuote& q, double target) { return q.strike < target; });

            for (; hit != quotes.end(); ++hit) {
                if (hit->first_date != entry_date) continue;
                if (hit->first_volume < min_entry_volume) continue;
                return &*hit;
            }
            return nullptr;
        }

        // Exact-strike lookup, for pricing the exit leg in the check month.
        const StrikeQuote* quote_for_strike(const ChainIndex& index, const std::string& key,
            double strike) {

            auto it = index.find(key);
            if (it == index.end()) return nullptr;

            const auto& quotes = it->second;
            auto hit = std::lower_bound(quotes.begin(), quotes.end(), strike - 1e-9,
                [](const StrikeQuote& q, double target) { return q.strike < target; });

            if (hit == quotes.end()) return nullptr;
            return (std::abs(hit->strike - strike) < 1e-9) ? &*hit : nullptr;
        }

        // Both ends of one contract's trade month: earliest close (entry mark) and
        // latest close (exit mark), tracked with the dates that produced them.
        struct MonthEnds {
            std::chrono::year_month_day first_date{};
            std::chrono::year_month_day last_date{};
            double first_close{};
            double last_close{};
            std::uint64_t first_volume{};
            std::chrono::year_month_day expiry{};
        };

        std::string date_of(const std::chrono::year_month_day& d) {
            return std::format("{:04d}-{:02d}-{:02d}", static_cast<int>(d.year()),
                static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
        }
    }

    namespace {
        // "YYYY-MM" shifted by whole months.
        std::string add_months(const std::string& month, int delta) {
            const int ordinal = std::stoi(month.substr(0, 4)) * 12
                              + std::stoi(month.substr(5, 2)) - 1 + delta;
            return std::format("{:04d}-{:02d}", ordinal / 12, ordinal % 12 + 1);
        }

        // Far enough past any listing convention to find the ceiling, small enough
        // that the probe stays trivial next to loading the chain.
        constexpr int kOffsetProbeLimit = 24;
    }

    int max_enterable_offset(const std::vector<MonthlyPrice>& prices, const ChainIndex& index) {
        int best = -1;
        for (const auto& price : prices) {
            for (int offset = kOffsetProbeLimit; offset > best; --offset) {
                const auto it = index.find(chain_key(price.month, add_months(price.month, offset)));
                if (it == index.end()) continue;
                for (const auto& quote : it->second) {
                    // Same rule the strategy enters on: quoted on the entry day itself.
                    if (quote.first_date == price.first_date) { best = offset; break; }
                }
            }
        }
        return best;
    }

    namespace {
        // Linear-interpolated percentile of an unsorted sample. Copies because the
        // caller's rolling window must keep its chronological order.
        double percentile_of(std::vector<double> sample, double percentile) {
            if (sample.empty()) return 0.0;
            std::sort(sample.begin(), sample.end());
            if (sample.size() == 1) return sample.front();
            const double rank = (percentile / 100.0) * static_cast<double>(sample.size() - 1);
            const auto lower = static_cast<std::size_t>(std::floor(rank));
            const auto upper = static_cast<std::size_t>(std::ceil(rank));
            const double weight = rank - static_cast<double>(lower);
            return sample[lower] * (1.0 - weight) + sample[upper] * weight;
        }
    }

    std::string chain_key(const std::string& trade_month, const std::string& expiry_month) {
        return trade_month + "|" + expiry_month;
    }

    ChainIndex build_chain_index(const std::vector<ChainRow>& chain) {
        std::unordered_map<std::string, std::map<double, MonthEnds>> acc;

        for (const auto& row : chain) {
            if (row.contract.right != Right::Call) continue;

            // Bucketed by BOTH the month it traded in and the month it expires in, so a
            // far-dated contract quoted today stays distinct from the front-month one.
            auto& by_strike = acc[chain_key(month_of(row.date), month_of(row.contract.expiry))];
            auto [it, inserted] = by_strike.try_emplace(row.contract.strike,
                MonthEnds{row.date, row.date, row.close, row.close,
                          row.total_traded_qty, row.contract.expiry});
            if (inserted) continue;

            MonthEnds& ends = it->second;
            if (row.date < ends.first_date) {
                ends.first_date = row.date;
                ends.first_close = row.close;
                ends.first_volume = row.total_traded_qty;
            }
            if (row.date > ends.last_date) {
                ends.last_date = row.date;
                ends.last_close = row.close;
            }
        }

        ChainIndex index;
        index.reserve(acc.size());
        for (const auto& [key, by_strike] : acc) {
            auto& quotes = index[key];
            quotes.reserve(by_strike.size());
            // std::map iterates strikes ascending, which is the order lookups rely on.
            for (const auto& [strike, ends] : by_strike) {
                quotes.push_back(StrikeQuote{strike, ends.first_close, ends.last_close,
                                             ends.expiry, ends.first_volume,
                                             date_of(ends.first_date)});
            }
        }
        return index;
    }

    DailyCloses build_daily_closes(const std::vector<EquityRow>& rows) {
        DailyCloses daily;
        daily.reserve(rows.size());
        for (const auto& row : rows) {
            daily[date_of(row.date)] = row.close;
        }
        return daily;
    }

    StrategyResult short_call_strategy(
        const std::vector<MonthlyPrice>& prices,
        const std::vector<ChainRow>& chain,
        const std::vector<EquityRow>& equity,
        double otm_pct,
        int month_offset,
        std::uint64_t min_entry_volume,
        const YieldFilter& yield_filter
    ) {
        return short_call_strategy(prices, build_chain_index(chain),
                                   build_daily_closes(equity), otm_pct, month_offset,
                                   min_entry_volume, yield_filter);
    }

    StrategyResult short_call_strategy(
        const std::vector<MonthlyPrice>& prices,
        const ChainIndex& index,
        const DailyCloses& daily,
        double otm_pct,
        int month_offset,
        std::uint64_t min_entry_volume,
        const YieldFilter& yield_filter
    ) {
        StrategyResult result = short_call_strategy(prices, otm_pct, month_offset);

        // Rolling window of recent candidate yields, oldest first. Only ever read
        // before the current cycle is appended, so the bar a cycle must clear is
        // set entirely by cycles that came before it.
        std::vector<double> recent_yields;
        const bool filtering = yield_filter.percentile > 0.0 && yield_filter.lookback > 0;

        // Looked up by month rather than by position: the pure overload skips any
        // entry whose calendar distance to the check month is not month_offset, so
        // outcomes and prices are no longer index-aligned.
        std::unordered_map<std::string, std::string> first_trading_day;
        first_trading_day.reserve(prices.size());
        for (const auto& p : prices) {
            first_trading_day.emplace(p.month, p.first_date);
        }

        for (auto& o : result.outcomes) {
            const auto entry_day_it = first_trading_day.find(o.entry_month);
            if (entry_day_it == first_trading_day.end()) continue;
            const std::string& entry_date = entry_day_it->second;

            // The contract sold is the one expiring in the check month, quoted during
            // the entry month. At offset 0 those are the same month; beyond that it is
            // a genuine far-dated contract rather than a different month's front month.
            // It must also have been quoted on the day the strike was chosen from.
            const StrikeQuote* quote = quote_at_or_above(
                index, chain_key(o.entry_month, o.check_month), o.strike, entry_date,
                min_entry_volume);
            if (!quote) continue;  // not listed at or above target on entry day; priced=false

            const double rounded_strike = quote->strike;
            const double premium = quote->first_close;

            o.rounded_strike = rounded_strike;
            o.premium = premium;
            o.entry_volume = quote->first_volume;
            o.entry_date = entry_date;
            o.expiry_date = date_of(quote->expiry);
            o.priced = true;

            // Exit is the SAME contract at its own expiry: same expiry month, but now
            // in the bucket where that month is also the trade month, i.e. its final
            // marks. Falling back to intrinsic keeps a row usable if it never traded
            // in its expiry month.
            const StrikeQuote* exit = quote_for_strike(
                index, chain_key(o.check_month, o.check_month), rounded_strike);

            // The verdict belongs on expiry day, not at a month boundary that can fall
            // days after the contract has already settled.
            const auto spot_it = daily.find(date_of(quote->expiry));
            if (spot_it != daily.end()) {
                o.check_price = spot_it->second;
            }
            o.held = o.check_price < rounded_strike;

            if (exit) {
                o.check_premium = exit->last_close;
                o.check_premium_is_market = true;
            } else {
                o.check_premium = std::max(0.0, o.check_price - rounded_strike);
                o.check_premium_is_market = false;
            }

            const double payoff = premium - o.check_premium;
            o.profit_pct = (o.entry_price != 0.0) ? (payoff / o.entry_price) * 100.0 : 0.0;

            o.premium_yield_pct = (o.entry_price != 0.0)
                ? (premium / o.entry_price) * 100.0 : 0.0;

            if (!filtering) {
                o.entered = true;
            } else if (recent_yields.size() < static_cast<std::size_t>(yield_filter.lookback)) {
                // Nothing to compare against yet. Passing these through would enter
                // them on no evidence and quietly inflate the filtered result with
                // trades the rule never actually approved.
                o.skip_reason = "no baseline";
                ++result.skipped_no_baseline;
            } else {
                o.yield_threshold_pct = percentile_of(recent_yields, yield_filter.percentile);
                if (o.premium_yield_pct >= o.yield_threshold_pct) {
                    o.entered = true;
                } else {
                    o.skip_reason = "premium below threshold";
                    ++result.skipped_low_yield;
                }
            }

            // Appended after the decision, and regardless of it: the baseline tracks
            // what premiums looked like, not what the filter chose to accept.
            if (filtering) {
                recent_yields.push_back(o.premium_yield_pct);
                if (recent_yields.size() > static_cast<std::size_t>(yield_filter.lookback)) {
                    recent_yields.erase(recent_yields.begin());
                }
            }
        }

        // Recount after the per-outcome held flags were updated.
        result.held_months = 0;
        for (const auto& o : result.outcomes) {
            if (o.held) ++result.held_months;
        }
        result.held_rate = (result.total_months > 0)
            ? static_cast<double>(result.held_months) / result.total_months
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
                    "rounded_strike,premium,check_premium,profit_pct\n";

            for (const auto& o : result.outcomes) {
                file << o.entry_month << ','
                     << o.entry_price << ','
                     << o.strike << ','
                     << o.check_month << ','
                     << o.check_price << ','
                     << (!o.priced ? "UNPRICED" : (o.held ? "HELD" : "BREACHED")) << ','
                     << o.rounded_strike << ','
                     << o.premium << ','
                     << o.check_premium << ','
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

    void run_short_call(std::string_view csv_path, double pct, int month_offset, std::string_view out_dir) {
        const auto rows = [&] {
            Timer t{"Loading CSV"};
            return load_equity_csv(std::string(csv_path));
        }();

        std::cout << "Loaded " << rows.size() << " rows." << std::endl;

        const auto monthly = aggregate_monthly_closes(rows);
        std::cout << "Aggregated into " << monthly.size() << " months\n";

        const auto result = short_call_strategy(monthly, pct, month_offset);

        for (const auto& o : result.outcomes) {
            std::cout << o.entry_month << " (entry " << o.entry_price
                      << ", strike " << o.strike << ") -> "
                      << o.check_month << " close " << o.check_price
                      << " -> " << (o.held ? "HELD" : "BREACHED") << "\n";
        }

        std::cout << (pct * 100.0) << "% strike (" << month_offset << "-month out) -> strike held: "
                  << result.held_rate << " ("
                  << result.held_months << "/" << result.total_months << ")\n";

        // Derive dynamic output filename from input path and parameters
        namespace fs = std::filesystem;
        const std::string stem = fs::path(std::string(csv_path)).stem().string();

        const long pct_int = std::lround(pct * 100.0);

        const std::string out_filename = stem
            + "_otm_pct" + std::to_string(pct_int)
            + "_monthly_offset" + std::to_string(month_offset)
            + ".csv";

        export_output_to_csv(result, out_dir, out_filename);
    }

    void run_short_call(std::string_view csv_path, std::string_view chain_csv_path, double pct,
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

        const auto result = short_call_strategy(monthly, chain, rows, pct, month_offset);

        for (const auto& o : result.outcomes) {
            std::cout << o.entry_month << " (entry " << o.entry_price
                      << ", strike " << o.strike << " -> rounded " << o.rounded_strike
                      << ", premium " << o.premium << "->" << o.check_premium
                      << ", profit " << o.profit_pct << "%) -> "
                      << o.check_month << " close " << o.check_price
                      << " -> " << (!o.priced ? "UNPRICED"
                                              : (o.held ? "HELD" : "BREACHED")) << "\n";
        }

        // Unpriced months carry the raw-strike verdict from the pure pass and zeros
        // everywhere else, so folding them into one rate blends two different
        // questions. Report the priced-only figure beside it rather than leaving a
        // single number that silently counts months in which nothing was traded.
        int priced = 0;
        int priced_held = 0;
        for (const auto& o : result.outcomes) {
            if (!o.priced) continue;
            ++priced;
            if (o.held) ++priced_held;
        }
        const double priced_rate = (priced > 0)
            ? static_cast<double>(priced_held) / priced : 0.0;

        std::cout << (pct * 100.0) << "% strike (" << month_offset << "-month out)\n"
                  << "  strike held, priced months only: " << priced_rate
                  << " (" << priced_held << "/" << priced << ")\n"
                  << "  all months incl. unpriced:       " << result.held_rate
                  << " (" << result.held_months << "/" << result.total_months << ")\n";

        namespace fs = std::filesystem;
        const std::string stem = fs::path(std::string(csv_path)).stem().string();

        const long pct_int = std::lround(pct * 100.0);

        const std::string out_filename = stem
            + "_otm_pct" + std::to_string(pct_int)
            + "_monthly_offset" + std::to_string(month_offset)
            + ".csv";

        export_output_to_csv(result, out_dir, out_filename);
    }

}
