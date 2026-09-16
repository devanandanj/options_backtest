//
// Covered-call position layer: lot sizes, currency P&L, assignment.
//

#include <gtest/gtest.h>
#include "../include/strategy/position.hpp"

using namespace backtester;

namespace {

    std::chrono::year_month_day ymd(const std::string& date) {
        return std::chrono::year{std::stoi(date.substr(0, 4))}
             / std::chrono::month{static_cast<unsigned>(std::stoi(date.substr(5, 2)))}
             / std::chrono::day{static_cast<unsigned>(std::stoi(date.substr(8, 2)))};
    }

    ChainRow lot_row(const std::string& date, int lot) {
        ChainRow row;
        row.contract.right = Right::Call;
        row.contract.strike = 100.0;
        row.contract.expiry = ymd(date.substr(0, 8) + "28");
        row.contract.lot_size = lot;
        row.date = ymd(date);
        return row;
    }

    // A priced outcome, spelled out so each test states only what it is about.
    MonthOutcome priced(const std::string& entry_month, const std::string& check_month,
                        double entry, double strike, double premium, double exit_spot) {
        MonthOutcome o;
        o.entry_month = entry_month;
        o.check_month = check_month;
        o.entry_price = entry;
        o.strike = strike;
        o.rounded_strike = strike;
        o.premium = premium;
        o.check_price = exit_spot;
        o.held = exit_spot < strike;
        o.priced = true;
        o.entered = true;   // these fixtures describe cycles that were opened
        return o;
    }

    StrategyResult resultOf(std::vector<MonthOutcome> outcomes) {
        StrategyResult r;
        r.total_months = static_cast<int>(outcomes.size());
        r.outcomes = std::move(outcomes);
        return r;
    }

    LotSizes fixedLot(std::uint64_t shares, const std::vector<std::string>& months) {
        LotSizes lots;
        for (const auto& m : months) lots[m] = LotSize{shares, true};
        return lots;
    }

}

TEST(LotSizes, TakesTheMonthsModalStatedLot) {
    // A revision lands mid-month; the lot the month mostly traded on wins.
    std::vector<ChainRow> chain = {
        lot_row("2024-07-01", 7500), lot_row("2024-07-02", 7500),
        lot_row("2024-07-03", 7500), lot_row("2024-07-29", 9275),
    };
    const auto lots = build_lot_sizes(chain);
    ASSERT_TRUE(lots.contains("2024-07"));
    EXPECT_EQ(lots.at("2024-07").shares, 7500u);
    EXPECT_TRUE(lots.at("2024-07").from_chain);
}

TEST(LotSizes, IgnoresRowsWithNoStatedLot) {
    // The legacy bhavcopy carries no lot column, so those rows arrive as zero and
    // must not be mistaken for a lot size of nothing.
    std::vector<ChainRow> chain = { lot_row("2022-01-03", 0), lot_row("2022-01-04", 0) };
    EXPECT_TRUE(build_lot_sizes(chain).empty());
}

TEST(Position, PremiumAndStockAreBothCountedInCurrency) {
    // One lot of 1000 bought at 100. The call expires worthless, so the whole
    // premium is kept and the shares are still held:
    //   premium  2.00 * 1000            = +2000
    //   stock    (104 - 100) * 1000     = +4000
    auto r = resultOf({ priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 104.0) });
    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01"}), PositionConfig{});

    ASSERT_EQ(pos.cycles.size(), 1u);
    const auto& c = pos.cycles[0];
    EXPECT_EQ(c.shares, 1000u);
    EXPECT_FALSE(c.assigned);
    EXPECT_DOUBLE_EQ(c.premium_collected, 2000.0);
    EXPECT_DOUBLE_EQ(c.pnl, 6000.0);
    EXPECT_EQ(pos.assignments, 0);
    EXPECT_EQ(pos.shares_held_at_end, 1000u);
}

TEST(Position, AssignmentCapsTheStockAtTheStrikeAndKeepsThePremium) {
    // Spot runs to 130 against a 110 strike. The shares are called away at 110,
    // so the upside above the strike is forgone but the premium is still kept:
    //   premium  2.00 * 1000        = +2000
    //   stock    (110 - 100) * 1000 = +10000   (not 30000)
    auto r = resultOf({ priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 130.0) });
    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01"}), PositionConfig{});

    ASSERT_EQ(pos.cycles.size(), 1u);
    const auto& c = pos.cycles[0];
    EXPECT_TRUE(c.assigned);
    EXPECT_DOUBLE_EQ(c.assignment_proceeds, 110000.0);
    EXPECT_DOUBLE_EQ(c.pnl, 12000.0);
    EXPECT_EQ(pos.assignments, 1);
    EXPECT_EQ(pos.shares_held_at_end, 0u);
    // Buy-and-hold caught the whole move; the covered call gave up the tail.
    EXPECT_DOUBLE_EQ(pos.benchmark_pnl, 30000.0);
}

TEST(Position, RebuysAfterAssignmentAtTheNextEntry) {
    // Called away in January at 110, then back in during February at 120 - the
    // gap between assignment and re-entry is time spent in cash while the stock ran.
    auto r = resultOf({
        priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 130.0),
        priced("2024-02", "2024-02", 120.0, 132.0, 3.0, 125.0),
    });
    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01", "2024-02"}), PositionConfig{});

    ASSERT_EQ(pos.cycles.size(), 2u);
    EXPECT_EQ(pos.cycles[1].shares, 1000u);
    EXPECT_DOUBLE_EQ(pos.cycles[1].entry_spot, 120.0);
    //   after cycle 1: +12000
    //   cycle 2: premium 3000, stock (125 - 120) * 1000 = 5000
    EXPECT_DOUBLE_EQ(pos.end_pnl, 20000.0);
    EXPECT_EQ(pos.assignments, 1);
}

TEST(Position, StaysInCashWhenRebuyIsDisabled) {
    auto r = resultOf({
        priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 130.0),
        priced("2024-02", "2024-02", 120.0, 132.0, 3.0, 125.0),
    });
    PositionConfig cfg;
    cfg.rebuy_after_assignment = false;
    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01", "2024-02"}), cfg);

    ASSERT_EQ(pos.cycles.size(), 1u);
    EXPECT_DOUBLE_EQ(pos.end_pnl, 12000.0);
}

TEST(Position, DoesNotOpenOverlappingCyclesAtLongerHolds) {
    // A two-month hold entered in January settles in March, so February and March
    // cannot also write a call: one lot of stock covers one contract. The next
    // entry is April.
    auto r = resultOf({
        priced("2024-01", "2024-03", 100.0, 110.0, 5.0, 104.0),
        priced("2024-02", "2024-04", 102.0, 112.0, 5.0, 104.0),
        priced("2024-03", "2024-05", 103.0, 113.0, 5.0, 104.0),
        priced("2024-04", "2024-06", 104.0, 114.0, 5.0, 106.0),
    });
    auto pos = simulate_covered_call(
        r, fixedLot(1000, {"2024-01", "2024-02", "2024-03", "2024-04"}), PositionConfig{});

    ASSERT_EQ(pos.cycles.size(), 2u);
    EXPECT_EQ(pos.cycles[0].entry_month, "2024-01");
    EXPECT_EQ(pos.cycles[1].entry_month, "2024-04");
}

TEST(Position, SkipsAndCountsMonthsWithNoKnownLotSize) {
    // The multiplier scales every currency figure, so an unknown lot is skipped
    // and reported rather than guessed.
    auto r = resultOf({
        priced("2022-01", "2022-01", 100.0, 110.0, 2.0, 104.0),
        priced("2024-07", "2024-07", 100.0, 110.0, 2.0, 104.0),
    });
    auto pos = simulate_covered_call(r, fixedLot(7500, {"2024-07"}), PositionConfig{});

    ASSERT_EQ(pos.cycles.size(), 1u);
    EXPECT_EQ(pos.cycles[0].entry_month, "2024-07");
    EXPECT_EQ(pos.skipped_unknown_lot, 1);
}

TEST(Position, OverrideFillsMonthsTheChainDoesNotState) {
    auto r = resultOf({ priced("2022-01", "2022-01", 100.0, 110.0, 2.0, 104.0) });
    PositionConfig cfg;
    cfg.lot_size_override = 12500;
    auto pos = simulate_covered_call(r, LotSizes{}, cfg);

    ASSERT_EQ(pos.cycles.size(), 1u);
    EXPECT_EQ(pos.cycles[0].shares, 12500u);
    EXPECT_EQ(pos.skipped_unknown_lot, 0);
    // The figures rest on an assumed multiplier, and say so.
    EXPECT_EQ(pos.cycles_on_override, 1);
}

TEST(Position, AStatedLotIsPreferredOverTheOverride) {
    // The override fills gaps; it never displaces a lot NSE actually published.
    auto r = resultOf({
        priced("2024-07", "2024-07", 100.0, 110.0, 2.0, 104.0),
        priced("2024-08", "2024-08", 104.0, 114.0, 2.0, 106.0),
    });
    LotSizes lots;
    lots["2024-07"] = LotSize{7500, true};
    PositionConfig cfg;
    cfg.lot_size_override = 1000;

    auto pos = simulate_covered_call(r, lots, cfg);
    ASSERT_EQ(pos.cycles.size(), 2u);
    EXPECT_EQ(pos.cycles[0].lot_size, 7500u);   // stated
    EXPECT_EQ(pos.cycles[1].lot_size, 1000u);   // filled
    EXPECT_EQ(pos.cycles_on_override, 1);
}

TEST(Position, LotsMultiplyTheWholePosition) {
    auto r = resultOf({ priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 104.0) });
    PositionConfig cfg;
    cfg.lots = 3;
    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01"}), cfg);

    ASSERT_EQ(pos.cycles.size(), 1u);
    EXPECT_EQ(pos.cycles[0].shares, 3000u);
    EXPECT_DOUBLE_EQ(pos.cycles[0].pnl, 18000.0);
}

TEST(Position, StopsWritingWhenARaisedLotExceedsTheHolding) {
    // NSE revises the lot from 1000 to 3000 while 1000 shares are held. One
    // contract can no longer be covered, so writing stops - counted, not silent,
    // because an uncounted skip looks exactly like the data running out.
    auto r = resultOf({
        priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 104.0),
        priced("2024-02", "2024-02", 104.0, 114.0, 2.0, 106.0),
    });
    LotSizes lots;
    lots["2024-01"] = LotSize{1000, true};
    lots["2024-02"] = LotSize{3000, true};

    auto pos = simulate_covered_call(r, lots, PositionConfig{});
    ASSERT_EQ(pos.cycles.size(), 1u);
    EXPECT_EQ(pos.skipped_under_one_lot, 1);
    EXPECT_EQ(pos.skipped_unknown_lot, 0);
}

TEST(Position, ARevisedLotChangesContractsWrittenNotSharesHeld) {
    // 2 lots of 1000 = 2000 shares. When the lot halves to 500, the same holding
    // covers four contracts instead of two; the share count does not move.
    auto r = resultOf({
        priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 104.0),
        priced("2024-02", "2024-02", 104.0, 114.0, 2.0, 106.0),
    });
    LotSizes lots;
    lots["2024-01"] = LotSize{1000, true};
    lots["2024-02"] = LotSize{500, true};
    PositionConfig cfg;
    cfg.lots = 2;

    auto pos = simulate_covered_call(r, lots, cfg);
    ASSERT_EQ(pos.cycles.size(), 2u);
    EXPECT_EQ(pos.cycles[0].contracts, 2u);
    EXPECT_EQ(pos.cycles[1].contracts, 4u);
    EXPECT_EQ(pos.cycles[0].shares, pos.cycles[1].shares);
}

TEST(Position, AssignmentTakesTheCoveredSharesAndRebuyRestoresTheHolding) {
    // 2000 shares against a 1000 lot: two contracts, fully covered. Assignment
    // takes all of them, and the next cycle buys the holding back at its entry.
    auto r = resultOf({
        priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 130.0),
        priced("2024-02", "2024-02", 130.0, 140.0, 2.0, 135.0),
    });
    LotSizes lots;
    lots["2024-01"] = LotSize{1000, true};
    lots["2024-02"] = LotSize{1000, true};
    PositionConfig cfg;
    cfg.lots = 2;   // 2000 shares held

    auto pos = simulate_covered_call(r, lots, cfg);
    ASSERT_EQ(pos.cycles.size(), 2u);
    EXPECT_EQ(pos.cycles[0].shares, 2000u);
    EXPECT_EQ(pos.cycles[0].contracts, 2u);
    EXPECT_EQ(pos.cycles[0].covered_shares, 2000u);
    EXPECT_TRUE(pos.cycles[0].assigned);
    // All 2000 were covered, so all 2000 are called away and then bought back.
    EXPECT_EQ(pos.cycles[1].shares, 2000u);
    EXPECT_DOUBLE_EQ(pos.cycles[1].entry_spot, 130.0);
}

TEST(Position, ExcessIsADifferenceSoALosingPeriodStillReadsCorrectly) {
    // The stock falls; the premium cushions it. Both P&Ls are negative, and the
    // strategy is ahead by exactly the premium it collected.
    auto r = resultOf({ priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 90.0) });
    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01"}), PositionConfig{});

    EXPECT_DOUBLE_EQ(pos.end_pnl, -8000.0);        // -10000 stock + 2000 premium
    EXPECT_DOUBLE_EQ(pos.benchmark_pnl, -10000.0);
    EXPECT_DOUBLE_EQ(pos.excess_pnl, 2000.0);      // ahead, despite losing money
}

TEST(Position, APricedCycleTheFilterRejectedNeverOpensAPosition) {
    // The contract existed and could be priced; the premium was simply too thin to
    // bother with. Counting it as a position would credit the strategy with a trade
    // its own entry rule declined to take.
    auto declined = priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 104.0);
    declined.entered = false;
    declined.skip_reason = "premium below threshold";

    auto pos = simulate_covered_call(resultOf({declined}), fixedLot(1000, {"2024-01"}),
                                     PositionConfig{});
    EXPECT_TRUE(pos.cycles.empty());
    EXPECT_DOUBLE_EQ(pos.end_pnl, 0.0);
}

TEST(Position, ACashSettledUnderlyingHasNoShareLegAtAll) {
    // An index cannot be owned, so there are no shares to hold, none to be called
    // away, and no buy-and-hold benchmark. Running the simulation anyway would
    // produce rupee figures for a position nobody could take - which look exactly
    // as credible as real ones.
    auto r = resultOf({
        priced("2024-01", "2024-01", 100.0, 110.0, 2.0, 104.0),
        priced("2024-02", "2024-02", 104.0, 114.0, 2.0, 130.0),
    });
    PositionConfig cfg;
    cfg.cash_settled = true;

    auto pos = simulate_covered_call(r, fixedLot(1000, {"2024-01", "2024-02"}), cfg);
    EXPECT_TRUE(pos.cycles.empty());
    EXPECT_DOUBLE_EQ(pos.end_pnl, 0.0);
    EXPECT_DOUBLE_EQ(pos.benchmark_pnl, 0.0);
    EXPECT_EQ(pos.assignments, 0);
    EXPECT_FALSE(pos.not_applicable.empty());   // and says why
}

TEST(Position, UnpricedOutcomesNeverOpenAPosition) {
    MonthOutcome blank;
    blank.entry_month = "2024-01";
    blank.check_month = "2024-01";
    auto pos = simulate_covered_call(resultOf({blank}), fixedLot(1000, {"2024-01"}),
                                     PositionConfig{});
    EXPECT_TRUE(pos.cycles.empty());
    EXPECT_DOUBLE_EQ(pos.end_pnl, 0.0);
}
