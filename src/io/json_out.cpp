//
// JSON serialization for backtest results.
//

#include "io/json_out.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

namespace backtester {

    namespace {
        using nlohmann::json;

        constexpr int kSchemaVersion = 1;

        // nlohmann throws on NaN/Inf. A degenerate result should still serialize,
        // so non-finite values become null instead of aborting the whole document.
        json num(double v) {
            return std::isfinite(v) ? json(v) : json(nullptr);
        }

        json outcome_to_json(const MonthOutcome& o) {
            return json{
                {"entry_month",    o.entry_month},
                {"entry_price",    num(o.entry_price)},
                {"strike",         num(o.strike)},
                {"check_month",    o.check_month},
                {"check_price",    num(o.check_price)},
                {"held",           o.held},
                {"rounded_strike", num(o.rounded_strike)},
                {"premium",        num(o.premium)},
                {"check_premium",  num(o.check_premium)},
                {"check_premium_is_market", o.check_premium_is_market},
                {"profit_pct",     num(o.profit_pct)},
                {"priced",         o.priced},
                {"entry_volume",   o.entry_volume},
                {"entry_date",     o.entry_date},
                {"expiry_date",    o.expiry_date},
                {"premium_yield_pct",   num(o.premium_yield_pct)},
                {"yield_threshold_pct", num(o.yield_threshold_pct)},
                {"entered",        o.entered},
                {"skip_reason",    o.skip_reason},
            };
        }

        json position_to_json(const PositionResult& pos) {
            json cycles = json::array();
            for (const auto& c : pos.cycles) {
                cycles.push_back(json{
                    {"entry_month",         c.entry_month},
                    {"check_month",         c.check_month},
                    {"shares",              c.shares},
                    {"lot_size",            c.lot_size},
                    {"contracts",           c.contracts},
                    {"covered_shares",      c.covered_shares},
                    {"entry_spot",          num(c.entry_spot)},
                    {"strike",              num(c.strike)},
                    {"exit_spot",           num(c.exit_spot)},
                    {"premium_collected",   num(c.premium_collected)},
                    {"assignment_proceeds", num(c.assignment_proceeds)},
                    {"assigned",            c.assigned},
                    {"cash",                num(c.cash)},
                    {"shares_value",        num(c.shares_value)},
                    {"pnl",                 num(c.pnl)},
                });
            }
            return json{
                {"cycles_run",          static_cast<int>(pos.cycles.size())},
                {"assignments",         pos.assignments},
                {"skipped_unknown_lot", pos.skipped_unknown_lot},
                {"skipped_under_one_lot", pos.skipped_under_one_lot},
                {"cycles_on_override",  pos.cycles_on_override},
                {"excess_pnl",          num(pos.excess_pnl)},
                {"end_pnl",             num(pos.end_pnl)},
                {"benchmark_pnl",       num(pos.benchmark_pnl)},
                {"shares_held_at_end",  pos.shares_held_at_end},
                {"cycles",              std::move(cycles)},
            };
        }

        json run_to_json(const RunResult& run) {
            int priced_months = 0;
            int traded_months = 0;
            int entered_months = 0;
            json outcomes = json::array();
            for (std::size_t i = 0; i < run.result.outcomes.size(); ++i) {
                const auto& o = run.result.outcomes[i];
                if (o.priced) ++priced_months;
                if (o.entered) ++entered_months;
                if (o.entered && o.entry_volume > 0) ++traded_months;

                json row = outcome_to_json(o);
                if (i < run.context.size()) {
                    const auto& c = run.context[i];
                    // Nested rather than flattened so it is obvious downstream which
                    // fields describe the run-in and which describe the trade.
                    row["context"] = json{
                        {"trailing_vol_pct", num(c.trailing_vol_pct)},
                        {"momentum_pct",     num(c.momentum_pct)},
                        {"range_pos_pct",    num(c.range_pos_pct)},
                        {"days_to_expiry",   c.days_to_expiry},
                        {"complete",         c.complete},
                    };
                }
                outcomes.push_back(std::move(row));
            }

            return json{
                {"otm_pct",       num(run.otm_pct)},
                {"month_offset",  run.month_offset},
                {"total_months",  run.result.total_months},
                {"held_months",   run.result.held_months},
                {"held_rate",     num(run.result.held_rate)},
                {"priced_months", priced_months},
                {"entered_months", entered_months},
                {"traded_months", traded_months},
                {"skipped_low_yield",   run.result.skipped_low_yield},
                {"skipped_no_baseline", run.result.skipped_no_baseline},
                {"outcomes",      std::move(outcomes)},
                {"position",      position_to_json(run.position)},
            };
        }
    }

    std::string to_json(const BacktestDocument& doc, bool pretty) {
        json runs = json::array();
        for (const auto& run : doc.runs) {
            runs.push_back(run_to_json(run));
        }

        json otm = json::array();
        for (double v : doc.otm_pcts) otm.push_back(num(v));

        json root{
            {"schema_version", kSchemaVersion},
            {"params", {
                {"equity_path",   doc.meta.equity_path},
                {"chain_path",    doc.meta.chain_path},
                {"otm_pcts",      std::move(otm)},
                {"month_offsets", doc.month_offsets},
                {"min_entry_volume", doc.min_entry_volume},
            }},
            {"meta", {
                {"equity_rows", doc.meta.equity_rows},
                {"chain_rows",  doc.meta.chain_rows},
                {"max_month_offset", doc.meta.max_month_offset},
                {"months",      doc.meta.months},
                {"load_ms",     doc.meta.load_ms},
                {"compute_ms",  doc.meta.compute_ms},
            }},
            {"runs", std::move(runs)},
        };

        return pretty ? root.dump(2) : root.dump();
    }

    std::string error_json(std::string_view message, bool pretty) {
        json root{
            {"schema_version", kSchemaVersion},
            {"error", std::string(message)},
        };
        return pretty ? root.dump(2) : root.dump();
    }

}
