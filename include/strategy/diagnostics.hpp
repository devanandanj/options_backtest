//
// What the market looked like on the day each trade was struck.
//
// The point of these is to answer "what do losing cycles have in common", and
// that question is only useful if the answer is visible BEFORE the trade. So
// every field here is computed from data available strictly before entry_date.
// Anything measured over the cycle itself — the realised move, how far past the
// strike it finished — is a description of the loss, not a warning of it, and is
// derived downstream where it can be labelled as such.
//
// Two dangers this guards against:
//
//  - Lookahead. A trailing window that includes the entry day's own close would
//    be harmless here, but one that ran past it would make every finding a
//    tautology. Windows end on the session before entry.
//  - Silent truncation. Early cycles have no 252 sessions of history behind them.
//    Rather than computing a range position from whatever exists and presenting
//    it beside a full-history one, short windows mark the feature incomplete.
//

#pragma once

#include <string>
#include <vector>

#include "data/equity_loader.hpp"
#include "strategy/short_call.hpp"

namespace backtester {

    struct EntryContext {
        // Annualised standard deviation of daily log returns over the sessions
        // before entry, in percent. How jumpy the stock had been going in.
        double trailing_vol_pct{};
        // Close-to-close change over the momentum window ending before entry, in
        // percent. Whether the stock arrived at the trade already running.
        double momentum_pct{};
        // Where entry sits between the trailing low and high, 0 = at the low,
        // 100 = at the high. A call written near the top of the range has less
        // room above it than the same percentage buffer written near the bottom.
        double range_pos_pct{};
        // Calendar days from entry to the contract's expiry.
        int days_to_expiry{};
        // False when the history behind entry_date was shorter than the windows
        // require, so the fields above would be computed from a partial sample.
        bool complete{};
    };

    struct DiagnosticWindows {
        int vol_sessions{20};
        int momentum_sessions{20};
        int range_sessions{252};
    };

    // Context for one date. `rows` must be ascending by date, as the loader returns.
    EntryContext entry_context(const std::vector<EquityRow>& rows,
                               const std::string& entry_date,
                               const std::string& expiry_date,
                               const DiagnosticWindows& windows = {});

}
