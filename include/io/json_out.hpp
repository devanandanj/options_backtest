//
// JSON serialization for backtest results.
//
// The nlohmann header is deliberately confined to json_out.cpp: it is ~900KB and
// including it here would push that cost onto every translation unit that wants
// to emit results.
//

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <cstdint>

#include "strategy/diagnostics.hpp"
#include "strategy/position.hpp"
#include "strategy/short_call.hpp"

namespace backtester {

    // One parameter combination and the result it produced.
    struct RunResult {
        double otm_pct{};
        int month_offset{};
        StrategyResult result{};
        PositionResult position{};   // empty cycles when the share leg was not run
        // Parallel to result.outcomes; empty when diagnostics were not computed.
        std::vector<EntryContext> context{};
    };

    struct RunMeta {
        std::string equity_path{};
        std::string chain_path{};   // empty when the run had no chain data
        std::size_t equity_rows{};
        std::size_t chain_rows{};
        std::size_t months{};
        long long load_ms{};
        long long compute_ms{};
        int max_month_offset{-1};   // furthest hold the chain can actually open
    };

    struct BacktestDocument {
        RunMeta meta{};
        std::vector<double> otm_pcts{};
        std::vector<int> month_offsets{};
        std::uint64_t min_entry_volume{};
        std::vector<RunResult> runs{};
    };

    // Serializes to the schema the HTTP layer consumes. Non-finite doubles are
    // emitted as null rather than throwing, so a degenerate result still produces
    // parseable output.
    std::string to_json(const BacktestDocument& doc, bool pretty = false);

    // Error payload, shaped so callers can parse stdout uniformly on success or failure.
    std::string error_json(std::string_view message, bool pretty = false);

}
