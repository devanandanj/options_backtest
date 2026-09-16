//
// Created by devanandan on 10-09-2026.
//
#include <gtest/gtest.h>
#include "../include/strategy/short_call.hpp"

using namespace backtester;

TEST(ShortCall, SameMonthAllHeld) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
        {"2024-02", 105.0, 105.0},
        {"2024-03", 108.0, 108.0},
    };
    auto result = short_call_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.total_months, 3);
    EXPECT_EQ(result.held_months, 3);
    EXPECT_DOUBLE_EQ(result.held_rate, 1.0);
}

TEST(ShortCall, SameMonthOneBreach) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},  // strike 110, check 100 -> held
        {"2024-02", 115.0, 115.0},  // strike 126.5, check 115 -> held
    };
    auto result = short_call_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.held_months, 2);
}

TEST(ShortCall, NextMonthOffset) {
    std::vector<MonthlyPrice> prices = {
        {"2024-09", 100.0, 100.0},  // strike 110, check Oct=120 -> breached
        {"2024-10", 120.0, 120.0},  // strike 132, check Nov=125 -> held
        {"2024-11", 125.0, 125.0},  // no Dec -> excluded
    };
    auto result = short_call_strategy(prices, 0.10, 1);
    EXPECT_EQ(result.total_months, 2);   // last month has no offset target
    EXPECT_EQ(result.held_months, 1);
    EXPECT_DOUBLE_EQ(result.held_rate, 0.5);
}

TEST(ShortCall, EmptyInput) {
    std::vector<MonthlyPrice> prices;
    auto result = short_call_strategy(prices, 0.10, 0);
    EXPECT_EQ(result.total_months, 0);
    EXPECT_DOUBLE_EQ(result.held_rate, 0.0);
}

TEST(ShortCall, EntryUsesMonthStartNotMonthEnd) {
    // The month opens at 100 and rallies to 150 by month end. The strike must be
    // derived from the 100 the trader could actually see at entry (-> 110), not
    // from the 150 that is only knowable in hindsight (-> 165).
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 150.0, 100.0},
    };
    auto result = short_call_strategy(prices, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_DOUBLE_EQ(o.entry_price, 100.0);
    EXPECT_DOUBLE_EQ(o.strike, 110.0);
    EXPECT_DOUBLE_EQ(o.check_price, 150.0);  // outcome still measured at month end
    EXPECT_FALSE(o.held);                 // 150 breached the 110 strike
}

namespace {
    std::chrono::year_month_day ymd(const std::string& date) {
        return std::chrono::year{std::stoi(date.substr(0, 4))}
             / std::chrono::month{static_cast<unsigned>(std::stoi(date.substr(5, 2)))}
             / std::chrono::day{static_cast<unsigned>(std::stoi(date.substr(8, 2)))};
    }

    // `expiry` empty means "expires at the end of its own trade month" — the
    // front-month case, which is what a zero month_offset trades.
    ChainRow make_call_row(const std::string& date, double strike, double close,
                           const std::string& expiry = "", std::uint64_t volume = 100) {
        ChainRow row;
        row.contract.right = Right::Call;
        row.contract.strike = strike;
        row.close = close;
        row.total_traded_qty = volume;
        row.date = ymd(date);
        row.contract.expiry = ymd(expiry.empty() ? (date.substr(0, 8) + "28") : expiry);
        return row;
    }

    EquityRow make_equity_row(const std::string& date, double close) {
        EquityRow row;
        row.date = ymd(date);
        row.close = close;
        return row;
    }

    // Most chain tests don't exercise the expiry-day verdict. With no close on file
    // for the expiry date, check_price keeps the month-end value the pure overload
    // assigned, which is exactly the behaviour those tests were written against.
    const std::vector<EquityRow> equity{};
}

TEST(ShortCall, RoundsUpToSmallestAvailableStrikeAtOrAboveTarget) {
    // entry_price 100, otm_pct 0.10 -> raw strike 110.0. OTM buffer requires
    // strike >= raw, so 108 is rejected; smallest available >= 110 is 112.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 108.0, 4.0),  // below target, must NOT be picked
        make_call_row("2024-01-01", 112.0, 2.0),  // earliest for 112
        make_call_row("2024-01-08", 112.0, 1.5),
        make_call_row("2024-01-01", 115.0, 1.0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_DOUBLE_EQ(o.strike, 110.0);
    EXPECT_DOUBLE_EQ(o.rounded_strike, 112.0);
    EXPECT_DOUBLE_EQ(o.premium, 2.0);         // sold on the 1st
    EXPECT_DOUBLE_EQ(o.check_premium, 1.5);   // bought back on the 8th, the last mark
    EXPECT_TRUE(o.check_premium_is_market);
    EXPECT_DOUBLE_EQ(o.profit_pct, 0.5);      // (2.0 - 1.5) / 100 * 100
}

TEST(ShortCall, PayoffIsTheRoundTripBetweenEntryAndExitMarks) {
    // Sold on the 1st for 6.0, bought back at the month's last mark of 1.25.
    // The 4.75 kept is the whole result — the underlying's path in between,
    // and whether the strike was breached, do not enter the arithmetic.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 130.0, 100.0, "2024-01-02"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-02", 110.0, 6.0),
        make_call_row("2024-01-16", 110.0, 3.0),
        make_call_row("2024-01-25", 110.0, 1.25),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_DOUBLE_EQ(o.premium, 6.0);
    EXPECT_DOUBLE_EQ(o.check_premium, 1.25);
    EXPECT_DOUBLE_EQ(o.profit_pct, 4.75);   // (6.0 - 1.25) / 100 * 100
    EXPECT_FALSE(o.held);                // check 130 did breach the 110 strike...
    EXPECT_GT(o.profit_pct, 0.0);           // ...yet the round trip still made money
}

TEST(ShortCall, FallsBackToIntrinsicWhenTheSoldContractNeverTradesInItsExpiryMonth) {
    // A February-expiry contract quoted in January: that is what offset 1 sells.
    // It leaves no February mark, so the exit settles at intrinsic instead:
    // max(0, 125 - 110) = 15.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
        {"2024-02", 125.0, 100.0, "2024-02-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "2024-02-29"),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 1);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_TRUE(o.priced);
    EXPECT_DOUBLE_EQ(o.premium, 3.0);
    EXPECT_DOUBLE_EQ(o.check_premium, 15.0);
    EXPECT_FALSE(o.check_premium_is_market);
    EXPECT_DOUBLE_EQ(o.profit_pct, -12.0);  // (3.0 - 15.0) / 100 * 100
}

TEST(ShortCall, MultiMonthHoldSellsAndSettlesTheSameContract) {
    // offset 2 from January sells the MARCH-expiry contract, quoted in January,
    // and closes it at that same contract's final March mark. The February-expiry
    // contract in the chain is a decoy: picking it up would be the cross-contract
    // bug this design exists to prevent.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 101.0, 100.0, "2024-01-01"},
        {"2024-02", 104.0, 102.0, "2024-02-01"},
        {"2024-03", 118.0, 105.0, "2024-03-01"},
    };
    std::vector<ChainRow> chain = {
        // The March contract, quoted in January (entry) and in March (exit).
        make_call_row("2024-01-01", 110.0, 7.00, "2024-03-28"),
        make_call_row("2024-03-28", 110.0, 2.50, "2024-03-28"),
        // Decoy: same strike, February expiry, much cheaper.
        make_call_row("2024-01-01", 110.0, 1.00, "2024-02-29"),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 2);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_EQ(o.entry_month, "2024-01");
    EXPECT_EQ(o.check_month, "2024-03");
    EXPECT_DOUBLE_EQ(o.premium, 7.00);        // the March contract, not the decoy
    EXPECT_DOUBLE_EQ(o.check_premium, 2.50);  // the SAME contract at its expiry
    EXPECT_TRUE(o.check_premium_is_market);
    EXPECT_DOUBLE_EQ(o.profit_pct, 4.5);      // (7.00 - 2.50) / 100 * 100
}

TEST(ShortCall, VerdictUsesSpotOnExpiryDayNotMonthEnd) {
    // The contract expires on the 25th with spot at 105, below the 110 strike, so
    // the strike held. Spot then runs to 130 by month end. Judging at month end
    // would call this a breach for a move the contract was never exposed to.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 130.0, 100.0, "2024-01-01"},   // month-end close 130
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "2024-01-25"),
        make_call_row("2024-01-25", 110.0, 0.05, "2024-01-25"),
    };
    const std::vector<EquityRow> with_expiry_close = {
        make_equity_row("2024-01-25", 105.0),   // spot on expiry day
    };

    auto result = short_call_strategy(prices, chain, with_expiry_close, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);

    const auto& o = result.outcomes[0];
    EXPECT_DOUBLE_EQ(o.check_price, 105.0);  // expiry-day spot, not the 130 month-end
    EXPECT_TRUE(o.held);                  // 105 < 110: the strike held
    EXPECT_DOUBLE_EQ(o.profit_pct, 2.95);    // (3.0 - 0.05) / 100 * 100
}

TEST(ShortCall, NeverPicksStrikeBelowTarget) {
    // entry_price 100, otm_pct 0.075 -> raw strike 107.5. 105 is nearer but
    // sits below the OTM target, so it must be rejected in favour of 110.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 105.0, 2.5),
        make_call_row("2024-01-01", 110.0, 1.0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.075, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 110.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 1.0);
}

TEST(ShortCall, RecordsEntryVolumeSoUntradedQuotesStayIdentifiable) {
    // The premium is a carried mark: the strike is listed but never trades. With no
    // minimum set the month is still priced, and entry_volume is what says so.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "", 0),
        make_call_row("2024-01-25", 110.0, 3.0, "", 0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_TRUE(result.outcomes[0].priced);
    EXPECT_EQ(result.outcomes[0].entry_volume, 0u);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);  // a frozen mark nets nothing
}

TEST(ShortCall, MinEntryVolumeClimbsToTheNextStrikeThatActuallyTraded) {
    // 110 clears the target but never traded; 115 did. The filter must move up to a
    // strike someone could have sold rather than abandoning an otherwise good month.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "", 0),
        make_call_row("2024-01-01", 115.0, 2.0, "", 250),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0, 1);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_TRUE(result.outcomes[0].priced);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 115.0);
    EXPECT_EQ(result.outcomes[0].entry_volume, 250u);
}

TEST(ShortCall, MinEntryVolumeLeavesTheMonthUnpricedWhenNothingTraded) {
    // Every qualifying strike is a carried mark, so there was no entry to make.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "", 0),
        make_call_row("2024-01-01", 115.0, 2.0, "", 0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0, 1);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_FALSE(result.outcomes[0].priced);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);
}

TEST(ShortCall, MinEntryVolumeHonoursAThresholdAboveOne) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "", 40),   // traded, but thinly
        make_call_row("2024-01-01", 115.0, 2.0, "", 500),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0, 100);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 115.0);
    EXPECT_EQ(result.outcomes[0].entry_volume, 500u);
}

TEST(ShortCall, MaxEnterableOffsetReportsTheFurthestHoldTheChainCanOpen) {
    // Three serial expiries quoted on the entry day, as NSE lists them, so the
    // furthest hold that can actually be opened is two months out.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
        {"2024-02", 100.0, 100.0, "2024-02-01"},
        {"2024-03", 100.0, 100.0, "2024-03-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "2024-01-25"),
        make_call_row("2024-01-01", 110.0, 4.0, "2024-02-29"),
        make_call_row("2024-01-01", 110.0, 5.0, "2024-03-28"),
    };

    EXPECT_EQ(max_enterable_offset(prices, build_chain_index(chain)), 2);
}

TEST(ShortCall, MaxEnterableOffsetIgnoresExpiriesListedAfterTheEntryDay) {
    // The April contract appears only late in January, once the front month has
    // expired. It is never sellable at entry, so it must not raise the ceiling.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 110.0, 3.0, "2024-01-25"),
        make_call_row("2024-01-29", 110.0, 6.0, "2024-04-25"),
    };

    EXPECT_EQ(max_enterable_offset(prices, build_chain_index(chain)), 0);
}

TEST(ShortCall, MaxEnterableOffsetIsNegativeWhenNothingIsQuoted) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    EXPECT_EQ(max_enterable_offset(prices, ChainIndex{}), -1);
}

TEST(ShortCall, SkipsEntriesWhoseCheckMonthIsNotTheRequestedDistanceAway) {
    // May is absent from the series, as it was when a data-feed gap dropped it.
    // Indexing by position would pair April with June and report that two-month
    // hold as offset 1. April must be skipped instead; March->April still stands.
    std::vector<MonthlyPrice> prices = {
        {"2026-03", 100.0, 100.0, "2026-03-02"},
        {"2026-04", 100.0, 100.0, "2026-04-01"},
        {"2026-06", 100.0, 100.0, "2026-06-01"},
    };

    auto result = short_call_strategy(prices, 0.10, 1);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_EQ(result.outcomes[0].entry_month, "2026-03");
    EXPECT_EQ(result.outcomes[0].check_month, "2026-04");
}

namespace {
    // `count` months, each entering at spot 100 with a 110 strike whose premium is
    // taken from `premiums` in order. Everything else is held constant so a test
    // varies only the thing it is about.
    struct YieldFixture {
        std::vector<MonthlyPrice> prices;
        std::vector<ChainRow> chain;
    };

    YieldFixture yieldFixture(const std::vector<double>& premiums) {
        YieldFixture f;
        for (std::size_t i = 0; i < premiums.size(); ++i) {
            const int month = static_cast<int>(i) + 1;
            char label[8], first[11], expiry[11];
            std::snprintf(label, sizeof label, "2024-%02d", month);
            std::snprintf(first, sizeof first, "2024-%02d-01", month);
            std::snprintf(expiry, sizeof expiry, "2024-%02d-25", month);
            f.prices.push_back({label, 100.0, 100.0, first});
            f.chain.push_back(make_call_row(first, 110.0, premiums[i], expiry));
            // A final mark so the exit prices from the market rather than intrinsic.
            f.chain.push_back(make_call_row(expiry, 110.0, 0.05, expiry));
        }
        return f;
    }
}

TEST(YieldFilter, OffByDefaultSoEveryPricedCycleIsEntered) {
    auto f = yieldFixture({1.0, 2.0, 3.0, 4.0});
    auto r = short_call_strategy(f.prices, f.chain, equity, 0.10, 0);

    ASSERT_EQ(r.outcomes.size(), 4u);
    for (const auto& o : r.outcomes) {
        EXPECT_TRUE(o.priced);
        EXPECT_TRUE(o.entered);
        EXPECT_TRUE(o.skip_reason.empty());
    }
    EXPECT_EQ(r.skipped_low_yield, 0);
}

TEST(YieldFilter, PricedButThinCyclesAreSkippedNotUnpriced) {
    // The distinction the whole refactor exists for: a contract was there and could
    // be priced, and the rule simply declined it. Marking it unpriced would blame
    // the data for a decision the strategy made.
    auto f = yieldFixture({5.0, 5.0, 5.0, 1.0});
    auto r = short_call_strategy(f.prices, f.chain, equity, 0.10, 0, 0, YieldFilter{50.0, 3});

    ASSERT_EQ(r.outcomes.size(), 4u);
    const auto& last = r.outcomes[3];
    EXPECT_TRUE(last.priced);
    EXPECT_FALSE(last.entered);
    EXPECT_EQ(last.skip_reason, "premium below threshold");
    EXPECT_EQ(r.skipped_low_yield, 1);
}

TEST(YieldFilter, ARichCycleClearsTheBaselineAndIsEntered) {
    auto f = yieldFixture({1.0, 1.0, 1.0, 9.0});
    auto r = short_call_strategy(f.prices, f.chain, equity, 0.10, 0, 0, YieldFilter{50.0, 3});

    ASSERT_EQ(r.outcomes.size(), 4u);
    EXPECT_TRUE(r.outcomes[3].entered);
    EXPECT_NEAR(r.outcomes[3].premium_yield_pct, 9.0, 1e-9);
    EXPECT_NEAR(r.outcomes[3].yield_threshold_pct, 1.0, 1e-9);
}

TEST(YieldFilter, CyclesBeforeTheWindowFillsAreSkippedForWantOfABaseline) {
    // Entering these on no evidence would credit the filtered run with trades the
    // rule never approved, and flatter it against the unfiltered one.
    auto f = yieldFixture({1.0, 2.0, 3.0, 4.0, 5.0});
    auto r = short_call_strategy(f.prices, f.chain, equity, 0.10, 0, 0, YieldFilter{50.0, 3});

    ASSERT_EQ(r.outcomes.size(), 5u);
    EXPECT_EQ(r.skipped_no_baseline, 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(r.outcomes[i].entered);
        EXPECT_EQ(r.outcomes[i].skip_reason, "no baseline");
    }
    EXPECT_TRUE(r.outcomes[3].entered);
    EXPECT_TRUE(r.outcomes[4].entered);
}

TEST(YieldFilter, TheBaselineTracksAllCandidatesNotOnlyAcceptedOnes) {
    // Feeding the window only its own survivors would ratchet the bar upward until
    // nothing qualified. Premiums here fall away after a rich opening run: with a
    // candidate-fed baseline the later thin cycles are judged against the real
    // recent history and, once it has decayed to their level, are entered again.
    auto f = yieldFixture({9.0, 9.0, 9.0, 1.0, 1.0, 1.0, 1.0});
    auto r = short_call_strategy(f.prices, f.chain, equity, 0.10, 0, 0, YieldFilter{50.0, 3});

    ASSERT_EQ(r.outcomes.size(), 7u);
    EXPECT_FALSE(r.outcomes[3].entered);            // 1.0 against a baseline of 9.0
    EXPECT_TRUE(r.outcomes[6].entered);             // baseline has decayed to 1.0
    EXPECT_NEAR(r.outcomes[6].yield_threshold_pct, 1.0, 1e-9);
}

TEST(YieldFilter, AHigherPercentileAdmitsFewerCycles) {
    // Premiums must oscillate for a percentile to discriminate at all: against a
    // monotonically rising series every cycle beats its whole window and clears any
    // threshold. The mid-sized 5s here are the ones the two settings disagree about.
    auto f = yieldFixture({1.0, 9.0, 1.0, 9.0, 1.0, 9.0, 5.0, 5.0, 5.0});
    auto lenient = short_call_strategy(f.prices, f.chain, equity, 0.10, 0, 0, YieldFilter{10.0, 3});
    auto strict = short_call_strategy(f.prices, f.chain, equity, 0.10, 0, 0, YieldFilter{90.0, 3});

    const auto entered = [](const StrategyResult& r) {
        int n = 0;
        for (const auto& o : r.outcomes) if (o.entered) ++n;
        return n;
    };
    EXPECT_GT(entered(lenient), entered(strict));
}

TEST(ShortCall, SkipsStrikesNotQuotedOnTheEntryDay) {
    // 112 is the smallest strike at or above the 110 target, but it is first quoted
    // on the 24th — three weeks after the strike was chosen from day-one spot. Taking
    // its premium would pair a day-1 strike with a day-24 price. 115 was quoted on
    // day one, so that is the trade; the alternative is a fictitious entry.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-24", 112.0, 2.0),
        make_call_row("2024-01-01", 115.0, 1.0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 115.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 1.0);
}

TEST(ShortCall, LeavesMonthUnpricedWhenNoQualifyingStrikeTradedOnEntryDay) {
    // Every strike at or above target was listed only later in the month — which is
    // exactly how a contract beyond NSE's three serial expiries appears in the data.
    // No entry was possible, so the month must stay unpriced rather than invent one.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-24", 112.0, 2.0),
        make_call_row("2024-01-29", 115.0, 1.0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_FALSE(result.outcomes[0].priced);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);
}

TEST(ShortCall, NoStrikeAboveTargetLeavesPremiumUnset) {
    // entry_price 100, otm_pct 0.10 -> raw 110. Chain only has 105 and 108
    // (both below target). Strategy should skip rather than round down into ITM.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 105.0, 5.0),
        make_call_row("2024-01-01", 108.0, 4.0),
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);
}

TEST(ShortCall, NoChainDataForMonthLeavesPremiumUnset) {
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-02-01", 110.0, 3.0),  // different month
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 1);
    EXPECT_DOUBLE_EQ(result.outcomes[0].rounded_strike, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].premium, 0.0);
    EXPECT_DOUBLE_EQ(result.outcomes[0].profit_pct, 0.0);
}

TEST(ShortCall, PricedFlagMarksMonthsThatCouldNotBeTraded) {
    // Jan has a strike above the 110 target, Feb does not. Without the flag the
    // Feb row's zeros are indistinguishable from a genuine break-even trade.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 100.0, 100.0, "2024-01-01"},
        {"2024-02", 100.0, 100.0, "2024-02-01"},
        {"2024-03", 100.0, 100.0, "2024-03-01"},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 112.0, 2.0),
        make_call_row("2024-02-01", 105.0, 6.0),  // below the 110 target
    };

    auto result = short_call_strategy(prices, chain, equity, 0.10, 0);
    ASSERT_EQ(result.outcomes.size(), 3);
    EXPECT_TRUE(result.outcomes[0].priced);
    EXPECT_FALSE(result.outcomes[1].priced);  // strike listed, but under target
    EXPECT_FALSE(result.outcomes[2].priced);  // no chain rows for March at all
}

TEST(ChainIndex, GroupsByMonthWithStrikesAscending) {
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-03", 115.0, 1.0),
        make_call_row("2024-01-03", 105.0, 5.0),
        make_call_row("2024-01-03", 110.0, 3.0),
        make_call_row("2024-02-05", 120.0, 2.0),
    };

    auto index = build_chain_index(chain);
    ASSERT_EQ(index.size(), 2u);
    ASSERT_TRUE(index.contains(chain_key("2024-01", "2024-01")));
    ASSERT_TRUE(index.contains(chain_key("2024-02", "2024-02")));

    const auto& jan = index.at(chain_key("2024-01", "2024-01"));
    ASSERT_EQ(jan.size(), 3u);
    EXPECT_DOUBLE_EQ(jan[0].strike, 105.0);
    EXPECT_DOUBLE_EQ(jan[1].strike, 110.0);
    EXPECT_DOUBLE_EQ(jan[2].strike, 115.0);
}

TEST(ChainIndex, KeepsCloseFromEarliestTradingDayOfMonth) {
    // Rows deliberately out of chronological order; the 1st must win regardless.
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-15", 110.0, 9.9),
        make_call_row("2024-01-01", 110.0, 4.0),
        make_call_row("2024-01-08", 110.0, 7.7),
    };

    auto index = build_chain_index(chain);
    const auto& jan = index.at(chain_key("2024-01", "2024-01"));
    ASSERT_EQ(jan.size(), 1u);
    EXPECT_DOUBLE_EQ(jan[0].first_close, 4.0);
}

TEST(ChainIndex, ExcludesPuts) {
    ChainRow put = make_call_row("2024-01-01", 110.0, 3.0);
    put.contract.right = Right::Put;

    auto index = build_chain_index({put});
    EXPECT_TRUE(index.empty());
}

TEST(ChainIndex, PrebuiltIndexMatchesRawChainOverload) {
    // The raw-chain overload delegates to the index one; this guards against the
    // two drifting apart if either is changed independently.
    std::vector<MonthlyPrice> prices = {
        {"2024-01", 118.0, 100.0},
        {"2024-02", 90.0, 104.0},
    };
    std::vector<ChainRow> chain = {
        make_call_row("2024-01-01", 112.0, 2.0),
        make_call_row("2024-01-09", 112.0, 1.4),
        make_call_row("2024-01-01", 120.0, 0.8),
        make_call_row("2024-02-01", 115.0, 3.1),
    };

    auto from_chain = short_call_strategy(prices, chain, equity, 0.10, 0);
    auto from_index = short_call_strategy(prices, build_chain_index(chain), build_daily_closes(equity), 0.10, 0);

    EXPECT_EQ(from_chain.total_months, from_index.total_months);
    EXPECT_EQ(from_chain.held_months, from_index.held_months);
    EXPECT_DOUBLE_EQ(from_chain.held_rate, from_index.held_rate);
    ASSERT_EQ(from_chain.outcomes.size(), from_index.outcomes.size());

    for (size_t i = 0; i < from_chain.outcomes.size(); ++i) {
        const auto& a = from_chain.outcomes[i];
        const auto& b = from_index.outcomes[i];
        EXPECT_EQ(a.entry_month, b.entry_month) << "at " << i;
        EXPECT_DOUBLE_EQ(a.entry_price, b.entry_price) << "at " << i;
        EXPECT_DOUBLE_EQ(a.strike, b.strike) << "at " << i;
        EXPECT_EQ(a.check_month, b.check_month) << "at " << i;
        EXPECT_DOUBLE_EQ(a.check_price, b.check_price) << "at " << i;
        EXPECT_EQ(a.held, b.held) << "at " << i;
        EXPECT_DOUBLE_EQ(a.rounded_strike, b.rounded_strike) << "at " << i;
        EXPECT_DOUBLE_EQ(a.premium, b.premium) << "at " << i;
        EXPECT_DOUBLE_EQ(a.check_premium, b.check_premium) << "at " << i;
        EXPECT_DOUBLE_EQ(a.profit_pct, b.profit_pct) << "at " << i;
        EXPECT_EQ(a.priced, b.priced) << "at " << i;
    }
}