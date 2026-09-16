//
// The share leg.
//
// short_call.hpp prices the option in isolation and reports P&L as a percentage
// of spot. That answers "was writing this call profitable", but not "did owning
// the stock and writing calls against it beat owning the stock" — which needs a
// real position: shares, a contract multiplier, currency, and assignment.
//
// Two modelling decisions worth stating plainly:
//
// 1. Cycles do not overlap. short_call_strategy scans every entry month
//    independently, so at month_offset >= 1 it reports several positions open at
//    once. Against a fixed share position that is not a trade anyone could place:
//    one lot of stock covers one call. The simulation therefore walks the
//    outcomes in order and, having entered a cycle that settles in month M, takes
//    its next entry no earlier than M + 1.
//
// 2. A breached strike means assignment, not a buy-back. At expiry the option has
//    no time value left, so closing it and being assigned cost the same premium
//    either way; the difference is the shares, which assignment takes away. That
//    is the behaviour the share leg exists to model.
//

#pragma once

#include <cstdint>
#include <string>
#include <string>
#include <unordered_map>
#include <vector>

#include "strategy/short_call.hpp"

namespace backtester {

    // Contract multiplier for one trade month, and where the number came from.
    //
    // NSE revises lot sizes as a stock's price drifts, so this is per-month rather
    // than a constant, and the legacy bhavcopy format does not carry it at all.
    // Rather than infer one, a month whose lot is unknown is skipped and counted:
    // the multiplier scales every currency figure, so a guess would quietly
    // misstate the whole result. (Turnover / (contracts x spot) does approximate
    // it, but measured against months where NSE states the lot it runs 4-8% high,
    // because turnover is struck at intraday prices rather than the close.)
    struct LotSize {
        std::uint64_t shares{};
        bool from_chain{};   // false = supplied by configuration
    };

    using LotSizes = std::unordered_map<std::string, LotSize>;   // "YYYY-MM" -> lot

    // Modal stated lot per trade month. Months the chain leaves blank are simply
    // absent; PositionConfig::lot_size_override decides what happens to those.
    LotSizes build_lot_sizes(const std::vector<ChainRow>& chain);

    struct PositionConfig {
        // Shares held is fixed for the whole run at lots * the first cycle's lot size.
        // NSE revises lot sizes, and a holding that silently resized with them would
        // stop being comparable to a buy-and-hold benchmark that does not. Contracts
        // written is therefore floor(shares / lot), and a revision changes how many
        // calls the same holding covers rather than how much stock is owned.
        std::uint64_t lots{1};
        std::uint64_t lot_size_override{0}; // used only where the chain states none
        bool rebuy_after_assignment{true};  // false parks the proceeds in cash
        // Set for a cash-settled underlying - an index. There are no shares to hold,
        // none to be called away, and "buy and hold" would mean an ETF carrying
        // tracking error and fees rather than the index itself. The simulation
        // returns empty rather than reporting a position nobody could take.
        bool cash_settled{false};
    };

    struct Cycle {
        std::string entry_month;
        std::string check_month;
        std::uint64_t shares{};          // shares held entering the cycle
        std::uint64_t lot_size{};        // contract multiplier that month
        std::uint64_t contracts{};       // floor(shares / lot_size)
        std::uint64_t covered_shares{};  // contracts * lot_size; the rest is unwritten
        double entry_spot{};
        double strike{};
        double exit_spot{};              // underlying close on the contract's expiry day
        double premium_collected{};      // currency, premium * shares
        double assignment_proceeds{};    // currency, strike * shares; 0 unless assigned
        bool assigned{};
        double cash{};                   // running, after this cycle
        double shares_value{};           // marked at exit_spot
        // cash + shares_value. Cash opens negative by the cost of the stock, so this
        // nets to P&L against a zero base rather than an account balance.
        double pnl{};
    };

    struct PositionResult {
        std::vector<Cycle> cycles;
        double end_pnl{};
        // Same shares, bought on the same day, marked at the same final price, simply
        // held. The number the strategy has to beat to have been worth running.
        double benchmark_pnl{};
        int assignments{};
        // Strategy minus benchmark. Reported as a difference, not a ratio: both can be
        // negative, and 0.93x of a loss is an outperformance, which a ratio hides.
        double excess_pnl{};
        // Entries passed over because no lot size was known for that month. Non-zero
        // means the currency figures cover less of the period than the percentages do.
        int skipped_unknown_lot{};
        // Entries passed over because the holding no longer covers a single contract -
        // NSE raised the lot above it. Silently truncating the run here would look
        // like the data simply ended.
        int skipped_under_one_lot{};
        // Cycles whose multiplier came from PositionConfig rather than the chain.
        // Non-zero means part of this result rests on an assumed lot size, which
        // scales its currency figures - worth saying out loud, not burying.
        int cycles_on_override{};
        std::uint64_t shares_held_at_end{};
        // Empty when the share leg ran. Otherwise says why it did not, so the caller
        // can explain the absence instead of showing zeros.
        std::string not_applicable{};
    };

    // Walks `result`'s outcomes in order, taking non-overlapping priced cycles.
    PositionResult simulate_covered_call(const StrategyResult& result,
                                         const LotSizes& lots,
                                         const PositionConfig& config);

}
