//
// JSON-emitting CLI front end for the backtest engine.
//
// Deliberately does NOT include timer.hpp or call run_short_call: both write to
// std::cout, and stdout here carries nothing but the JSON document. Diagnostics
// go to std::cerr, gated behind --verbose.
//
// Exit codes: 0 ok, 1 runtime error, 2 bad arguments.
//

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "data/equity_loader.hpp"
#include "data/loader.hpp"
#include "data/monthly_aggregate.hpp"
#include "io/json_out.hpp"
#include "strategy/diagnostics.hpp"
#include "strategy/position.hpp"
#include "strategy/short_call.hpp"

namespace {

    constexpr int kExitOk = 0;
    constexpr int kExitRuntimeError = 1;
    constexpr int kExitBadArgs = 2;

    constexpr int kDefaultMaxRuns = 200;

    struct Options {
        std::string equity_path;
        std::string chain_path;          // optional
        std::vector<double> otm_pcts{0.10};
        std::vector<int> month_offsets{1};
        bool pretty = false;
        bool verbose = false;
        int max_runs = kDefaultMaxRuns;
        std::uint64_t min_volume = 0;
        std::uint64_t lots = 1;
        std::uint64_t lot_size = 0;    // override where the chain states none
        bool rebuy = true;
        double yield_pct = 0.0;   // 0 disables the premium filter
        int yield_lookback = 12;
    };

    struct BadArgs : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    void print_usage(std::ostream& os) {
        os << "usage: backtest_cli --equity PATH [--chain PATH]\n"
              "                    [--otm LIST] [--offset LIST]\n"
              "                    [--pretty] [--verbose] [--max-runs N]\n"
              "\n"
              "  --equity PATH   underlying daily-bar CSV (required)\n"
              "  --chain PATH    option-chain CSV; without it premiums are unpriced\n"
              "  --otm LIST      comma-separated OTM fractions, e.g. 0.05,0.10 (default 0.10)\n"
              "  --offset LIST   comma-separated month offsets, e.g. 0,1,2 (default 1)\n"
              "  --pretty        indent the JSON output\n"
              "  --verbose       progress diagnostics on stderr\n"
              "  --max-runs N    cap on the parameter grid (default 200)\n"
              "  --min-volume N  require the sold strike to have traded >= N contracts\n"
              "                  on the entry day; 0 (default) keeps untraded quotes\n"
              "  --lots N        lots of stock held; one call is written per lot\n"
              "  --lot-size N    contract multiplier for months the chain omits one\n"
              "  --no-rebuy      after assignment stay in cash instead of buying back\n"
              "  --yield-pct P   enter only when the premium yield clears the Pth\n"
              "                  percentile of recent cycles; 0 (default) enters always\n"
              "  --yield-lookback N  cycles in that baseline window (default 12)\n"
              "\n"
              "Emits one JSON document on stdout; --otm x --offset runs the full grid.\n";
    }

    std::vector<std::string> split_list(std::string_view s) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t comma = s.find(',', start);
            const size_t end = (comma == std::string_view::npos) ? s.size() : comma;
            std::string_view piece = s.substr(start, end - start);
            if (!piece.empty()) parts.emplace_back(piece);
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return parts;
    }

    double parse_double(const std::string& text, std::string_view flag) {
        try {
            size_t consumed = 0;
            const double value = std::stod(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception&) {
            throw BadArgs(std::string(flag) + ": not a number: '" + text + "'");
        }
    }

    int parse_int(const std::string& text, std::string_view flag) {
        try {
            size_t consumed = 0;
            const int value = std::stoi(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception&) {
            throw BadArgs(std::string(flag) + ": not an integer: '" + text + "'");
        }
    }

    long long parse_long_long(const std::string& text, std::string_view flag) {
        try {
            size_t consumed = 0;
            const long long value = std::stoll(text, &consumed);
            if (consumed != text.size()) throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception&) {
            throw BadArgs(std::string(flag) + ": not an integer: '" + text + "'");
        }
    }

    std::string_view require_value(int argc, char** argv, int& i, std::string_view flag) {
        if (i + 1 >= argc) throw BadArgs(std::string(flag) + " requires a value");
        return argv[++i];
    }

    Options parse_args(int argc, char** argv) {
        Options opt;
        bool otm_set = false;
        bool offset_set = false;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];

            if (arg == "--equity") {
                opt.equity_path = require_value(argc, argv, i, arg);
            } else if (arg == "--chain") {
                opt.chain_path = require_value(argc, argv, i, arg);
            } else if (arg == "--otm") {
                opt.otm_pcts.clear();
                for (const auto& piece : split_list(require_value(argc, argv, i, arg))) {
                    opt.otm_pcts.push_back(parse_double(piece, arg));
                }
                otm_set = true;
            } else if (arg == "--offset") {
                opt.month_offsets.clear();
                for (const auto& piece : split_list(require_value(argc, argv, i, arg))) {
                    opt.month_offsets.push_back(parse_int(piece, arg));
                }
                offset_set = true;
            } else if (arg == "--min-volume") {
                const long long v = parse_long_long(
                    std::string(require_value(argc, argv, i, arg)), arg);
                if (v < 0) throw BadArgs("--min-volume must not be negative");
                opt.min_volume = static_cast<std::uint64_t>(v);
            } else if (arg == "--lots") {
                const long long v = parse_long_long(
                    std::string(require_value(argc, argv, i, arg)), arg);
                if (v < 1) throw BadArgs("--lots must be at least 1");
                opt.lots = static_cast<std::uint64_t>(v);
            } else if (arg == "--lot-size") {
                const long long v = parse_long_long(
                    std::string(require_value(argc, argv, i, arg)), arg);
                if (v < 0) throw BadArgs("--lot-size must not be negative");
                opt.lot_size = static_cast<std::uint64_t>(v);
            } else if (arg == "--no-rebuy") {
                opt.rebuy = false;
            } else if (arg == "--yield-pct") {
                opt.yield_pct = parse_double(
                    std::string(require_value(argc, argv, i, arg)), arg);
                if (opt.yield_pct < 0.0 || opt.yield_pct > 100.0) {
                    throw BadArgs("--yield-pct must be between 0 and 100");
                }
            } else if (arg == "--yield-lookback") {
                opt.yield_lookback = parse_int(
                    std::string(require_value(argc, argv, i, arg)), arg);
                if (opt.yield_lookback < 1) throw BadArgs("--yield-lookback must be at least 1");
            } else if (arg == "--max-runs") {
                opt.max_runs = parse_int(std::string(require_value(argc, argv, i, arg)), arg);
            } else if (arg == "--pretty") {
                opt.pretty = true;
            } else if (arg == "--verbose") {
                opt.verbose = true;
            } else if (arg == "--help" || arg == "-h") {
                print_usage(std::cout);
                std::exit(kExitOk);
            } else {
                throw BadArgs("unknown argument: " + std::string(arg));
            }
        }

        if (opt.equity_path.empty()) throw BadArgs("--equity is required");
        if (otm_set && opt.otm_pcts.empty()) throw BadArgs("--otm was empty");
        if (offset_set && opt.month_offsets.empty()) throw BadArgs("--offset was empty");
        if (opt.max_runs < 1) throw BadArgs("--max-runs must be at least 1");

        for (int offset : opt.month_offsets) {
            if (offset < 0) throw BadArgs("--offset must not be negative");
        }

        const auto combos = opt.otm_pcts.size() * opt.month_offsets.size();
        if (combos > static_cast<size_t>(opt.max_runs)) {
            throw BadArgs("grid of " + std::to_string(combos) + " runs exceeds --max-runs "
                          + std::to_string(opt.max_runs));
        }

        return opt;
    }

    long long ms_since(std::chrono::steady_clock::time_point start) {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    }

}

int main(int argc, char** argv) {
    using namespace backtester;

    Options opt;
    try {
        opt = parse_args(argc, argv);
    } catch (const BadArgs& e) {
        std::cerr << "error: " << e.what() << "\n\n";
        print_usage(std::cerr);
        return kExitBadArgs;
    }

    try {
        const auto load_start = std::chrono::steady_clock::now();

        if (opt.verbose) std::cerr << "loading equity: " << opt.equity_path << '\n';
        const auto equity_rows = load_equity_csv(opt.equity_path);

        std::vector<ChainRow> chain;
        if (!opt.chain_path.empty()) {
            if (opt.verbose) std::cerr << "loading chain: " << opt.chain_path << '\n';
            chain = load_chain_csv(opt.chain_path);
        }

        const auto monthly = aggregate_monthly_closes(equity_rows);
        const ChainIndex index = chain.empty() ? ChainIndex{} : build_chain_index(chain);
        const DailyCloses daily = build_daily_closes(equity_rows);
        const LotSizes lot_sizes = build_lot_sizes(chain);
        const bool cash_settled = is_index_series(opt.equity_path);
        const long long load_ms = ms_since(load_start);

        if (opt.verbose) {
            std::cerr << "equity rows: " << equity_rows.size()
                      << ", chain rows: " << chain.size()
                      << ", months: " << monthly.size()
                      << ", load: " << load_ms << "ms\n";
        }

        const auto compute_start = std::chrono::steady_clock::now();
        std::vector<RunResult> runs;
        runs.reserve(opt.otm_pcts.size() * opt.month_offsets.size());

        for (const double otm : opt.otm_pcts) {
            for (const int offset : opt.month_offsets) {
                if (opt.verbose) {
                    std::cerr << "run otm=" << otm << " offset=" << offset << '\n';
                }
                StrategyResult result = opt.chain_path.empty()
                    ? short_call_strategy(monthly, otm, offset)
                    : short_call_strategy(monthly, index, daily, otm, offset, opt.min_volume,
                                          YieldFilter{opt.yield_pct, opt.yield_lookback});
                PositionConfig pos_cfg;
                pos_cfg.lots = opt.lots;
                pos_cfg.lot_size_override = opt.lot_size;
                pos_cfg.rebuy_after_assignment = opt.rebuy;
                // An index has no share leg. Detected from the data rather than
                // configured, so a new index dataset cannot be added without it.
                pos_cfg.cash_settled = cash_settled;
                PositionResult position = simulate_covered_call(result, lot_sizes, pos_cfg);

                // What the market looked like walking into each trade. Computed here
                // rather than inside the strategy: it changes no decision the engine
                // makes, it only describes the conditions the decision was made under.
                std::vector<EntryContext> context;
                context.reserve(result.outcomes.size());
                for (const auto& o : result.outcomes) {
                    context.push_back(entry_context(equity_rows, o.entry_date, o.expiry_date));
                }

                runs.push_back(RunResult{otm, offset, std::move(result),
                                         std::move(position), std::move(context)});
            }
        }
        const long long compute_ms = ms_since(compute_start);

        BacktestDocument doc;
        doc.meta = RunMeta{
            opt.equity_path,
            opt.chain_path,
            equity_rows.size(),
            chain.size(),
            monthly.size(),
            load_ms,
            compute_ms,
            index.empty() ? -1 : max_enterable_offset(monthly, index),
        };
        doc.min_entry_volume = opt.min_volume;
        doc.otm_pcts = opt.otm_pcts;
        doc.month_offsets = opt.month_offsets;
        doc.runs = std::move(runs);

        std::cout << to_json(doc, opt.pretty) << std::endl;
        return kExitOk;

    } catch (const std::exception& e) {
        // Error payload goes to stdout too, so the caller parses one format either way.
        std::cout << error_json(e.what(), opt.pretty) << std::endl;
        std::cerr << "error: " << e.what() << '\n';
        return kExitRuntimeError;
    }
}
