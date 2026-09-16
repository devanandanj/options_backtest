//
// Created by devanandan on 10-09-2026.
//

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "data/equity_loader.hpp"
#include "data/loader.hpp"

namespace backtester {

    struct MonthlyPrice {
        std::string month{};
        double close{};             // close on the last trading day of the month
        double first_close{};       // close on the first trading day of the month
        std::string first_date{};   // "YYYY-MM-DD" of that first trading day
    };

    struct MonthOutcome {
        std::string entry_month;    // e.g. "2024-09"
        double entry_price;
        double strike;
        std::string check_month;    // e.g. "2024-10"
        double check_price;
        // True when the underlying closed below the strike on expiry day. Says the
        // strike was not breached, NOT that the trade made money: a breach still
        // profits when the premium collected exceeds how far the option finished ITM.
        bool held;
        // Smallest Call strike listed in the chain for entry_month that is >= strike.
        // Zero when no listed strike reached the target, i.e. the month is unpriced.
        double rounded_strike{};
        double premium{};           // option close on the earliest trading day of entry_month
        // What it costs to close the position: the same strike's close on the latest
        // trading day of check_month. Falls back to intrinsic value when that month
        // lists no such strike (see check_premium_is_market).
        double check_premium{};
        // True when check_premium came from a real quote, false when it was derived
        // from intrinsic value as a fallback.
        bool check_premium_is_market{};
        // Round-trip short-call payoff as a percentage of entry_price:
        //   (premium - check_premium) / entry_price * 100
        double profit_pct{};
        // False when no strike at or above the target was listed that month, so no
        // trade could be priced. Such rows carry zeros in the three fields above and
        // must be excluded from aggregate statistics rather than read as break-even.
        bool priced{};
        // Contracts actually traded in the sold strike on the entry day. Zero means
        // the premium above is a carried settlement mark rather than a price anyone
        // transacted at, which is common in far-dated strikes: they can hold a close
        // frozen across every day they are listed, so entry and exit agree exactly
        // and the month reports a P&L that was never available. Always populated;
        // filtering on it is min_entry_volume's job.
        std::uint64_t entry_volume{};
        // Both dates are needed to place a cycle in time: the diagnostics measure
        // what the market looked like on entry_date, and days-to-expiry is the gap
        // between the two. The month labels alone cannot give either.
        std::string entry_date{};    // "YYYY-MM-DD", the day the trade was struck
        std::string expiry_date{};   // "YYYY-MM-DD", the contract's own expiry

        // Premium as a percentage of spot: how much the option paid relative to what
        // the underlying costs. This is the quantity "high premium" refers to.
        double premium_yield_pct{};
        // What premium_yield_pct had to clear, drawn from the preceding cycles. Zero
        // when the filter is off or no baseline had accumulated yet.
        double yield_threshold_pct{};
        // True when this cycle was actually opened. Distinct from `priced`, which
        // only says a contract existed to price: a cycle can be perfectly priceable
        // and still be passed over for paying too little. Statistics about the
        // strategy count entered cycles; statistics about data coverage count priced
        // ones, and collapsing the two would hide exactly what the filter did.
        bool entered{};
        // Why a priced cycle was not entered. Empty when it was.
        std::string skip_reason{};
    };

    struct StrategyResult {
        int total_months;
        int held_months;
        double held_rate;
        std::vector<MonthOutcome> outcomes;
        // Priced cycles the yield filter turned away, and those that arrived before
        // enough history had accumulated to judge them against.
        int skipped_low_yield{};
        int skipped_no_baseline{};
    };

    // Entry filter on how rich the premium is.
    //
    // A cycle is entered only when its premium yield clears the given percentile of
    // the preceding `lookback` cycles' yields. Self-referential on purpose: what
    // counts as a fat premium in a calm year is ordinary in a volatile one, so a
    // fixed "2% is high" would mean different things at different times.
    //
    // The baseline is built from every priced candidate, not only from cycles that
    // were entered. Feeding it the survivors would make it a record of what the
    // filter already accepted, ratcheting the bar upward until nothing qualifies.
    struct YieldFilter {
        // 0 disables the filter entirely; 60 means "richer than 60% of recent cycles".
        double percentile{0.0};
        int lookback{12};
    };

    StrategyResult short_call_strategy( const std::vector<MonthlyPrice>& prices, double otm_pct, int month_offset);

    // One Call strike of one expiry, as quoted during one trade month.
    struct StrikeQuote {
        double strike{};
        double first_close{};                  // close on the earliest trading day of the trade month
        double last_close{};                   // close on the latest trading day of the trade month
        std::chrono::year_month_day expiry{};  // the contract's actual expiry date
        std::uint64_t first_volume{};          // contracts traded on that earliest day
        // The day first_close came from. Not always the month's first trading day: a
        // contract only listed part-way through the month first quotes part-way through
        // it. Entry requires these to coincide, or strike and premium would be struck
        // weeks apart — see short_call_strategy.
        std::string first_date{};
    };

    // (trade month, expiry month) -> that bucket's Call strikes, ascending.
    //
    // The expiry dimension is what makes a multi-month hold expressible: selling a
    // contract N months out means quoting it in the entry month but settling it in
    // its own expiry month, which are different buckets of the same index.
    //
    // Key is "TRADE|EXPIRY", e.g. "2024-01|2024-03"; use chain_key() to build it.
    using ChainIndex = std::unordered_map<std::string, std::vector<StrikeQuote>>;

    std::string chain_key(const std::string& trade_month, const std::string& expiry_month);

    ChainIndex build_chain_index(const std::vector<ChainRow>& chain);

    // Underlying close by date, needed to judge whether the strike held on the
    // contract's expiry day rather than at some unrelated month boundary.
    using DailyCloses = std::unordered_map<std::string, double>;   // "YYYY-MM-DD" -> close

    DailyCloses build_daily_closes(const std::vector<EquityRow>& rows);

    // The largest month_offset any entry month could actually open a position at:
    // the furthest expiry that is quoted on some month's FIRST trading day, which is
    // the only day an entry may be struck. Exchanges list a fixed number of serial
    // expiries - NSE stock options list three - so this is typically 2, and asking
    // for more is not a failure but an instrument that does not exist. Returns -1
    // when the chain prices nothing at all.
    int max_enterable_offset(const std::vector<MonthlyPrice>& prices, const ChainIndex& index);

    // Prices a real short-call round trip against the chain.
    //
    // For entry month i the contract sold is the one expiring in month i+month_offset,
    // so month_offset is a genuine holding period rather than a comparison window:
    //   entry  — that contract's close on the first trading day of month i
    //   exit   — the SAME contract's close on the last trading day of its expiry month
    //   verdict— underlying close on the contract's expiry date, against the strike
    //
    // The target strike is rounded UP to the smallest listed strike at or above it;
    // rounding to nearest could land below spot and turn an OTM call into an ITM one.
    // Months where no such strike was listed are left unpriced (MonthOutcome::priced).
    //
    // The chosen strike must also have been quoted on the month's first trading day —
    // the same day entry_price, and therefore the target strike, is taken from. Without
    // that rule a contract first listed late in the month is entered at a premium struck
    // weeks after its own strike was chosen. NSE lists three serial expiries, so this
    // also means month_offset beyond 2 correctly prices nothing: those contracts do not
    // exist at entry.
    //
    // min_entry_volume, when non-zero, additionally requires the strike to have traded
    // at least that many contracts on the entry day, skipping upward to the next strike
    // otherwise. Far-dated strikes are frequently quoted without ever trading, and their
    // carried marks yield premiums nobody could have sold at; leaving it 0 prices those
    // months anyway and reports MonthOutcome::entry_volume so they stay identifiable.
    StrategyResult short_call_strategy( const std::vector<MonthlyPrice>& prices, const std::vector<ChainRow>& chain,
        const std::vector<EquityRow>& equity, double otm_pct, int month_offset,
        std::uint64_t min_entry_volume = 0, const YieldFilter& yield_filter = {});

    // Same, reusing prebuilt lookups. Preferred across a parameter sweep so the index
    // is built once rather than per combination.
    StrategyResult short_call_strategy( const std::vector<MonthlyPrice>& prices, const ChainIndex& index,
        const DailyCloses& daily, double otm_pct, int month_offset,
        std::uint64_t min_entry_volume = 0, const YieldFilter& yield_filter = {});

    void export_output_to_csv(const StrategyResult& result, std::string_view out_dir, std::string_view filename);

    void run_short_call(std::string_view csv_path, double pct, int month_offset,
        std::string_view out_dir = "out/short_call");

    void run_short_call(std::string_view csv_path, std::string_view chain_csv_path, double pct, int month_offset,
        std::string_view out_dir = "out/short_call");

}