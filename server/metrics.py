"""Derived statistics for a single engine run.

Lives in Python rather than C++ because these definitions are the part most
likely to change, and the inputs are ~50 rows so there is nothing to optimise.

Everything here is computed over PRICED outcomes only. A month where no listed
strike reached the target produced no trade; counting its zeros as a break-even
would quietly drag every average toward zero.
"""

from __future__ import annotations

import math
import statistics
from typing import Any

from . import diagnose

# Below this many cycles, a Sharpe or Sortino figure is noise dressed as a number.
# Thirty is the usual rule-of-thumb floor for any distributional statistic, and a
# monthly strategy reaches it only after two and a half years. Stated rather than
# silently applied, because a ratio computed on ten observations looks exactly as
# authoritative as one computed on three thousand.
MIN_CYCLES_FOR_RATIOS = 30


def _priced(outcomes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Cycles that actually opened a position.

    Named for history: it once meant "a contract existed to price". Since the yield
    filter arrived those are different populations - a cycle can be priceable and
    still declined - and every statistic about the strategy wants the entered one.
    Coverage reporting, which asks what the data supported, still counts `priced`.
    """
    return [o for o in outcomes if o.get("entered")]


def equity_curve(outcomes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Running sum of profit_pct over priced months.

    A running SUM, not a compounding product: profit_pct is a percentage of that
    month's entry price, and adding them assumes fixed position sizing with no
    reinvestment. Labelled in percentage points for that reason.
    """
    curve: list[dict[str, Any]] = []
    running = 0.0
    for outcome in _priced(outcomes):
        running += outcome.get("profit_pct") or 0.0
        curve.append({
            "month": outcome["entry_month"],
            "profit_pct": outcome.get("profit_pct") or 0.0,
            "cumulative_pct": running,
        })
    return curve


def max_drawdown_pct(curve: list[dict[str, Any]]) -> float:
    """Largest peak-to-trough fall of the cumulative curve, in percentage points.

    Not a ratio — the curve is a sum of percentages, so dividing by a running peak
    that can be zero or negative would be meaningless.
    """
    peak = 0.0
    worst = 0.0
    for point in curve:
        value = point["cumulative_pct"]
        peak = max(peak, value)
        worst = max(worst, peak - value)
    return worst


def summarize(run: dict[str, Any]) -> dict[str, Any]:
    """Summary statistics for one parameter combination."""
    outcomes: list[dict[str, Any]] = run.get("outcomes") or []
    priced = _priced(outcomes)
    total_months = int(run.get("total_months") or 0)

    curve = equity_curve(outcomes)
    profits = [o.get("profit_pct") or 0.0 for o in priced]
    wins = [p for p in profits if p > 0]
    losses = [p for p in profits if p < 0]

    # Strike held, per the engine's own flag, restricted to months that traded.
    held = sum(1 for o in priced if o.get("held"))
    traded = sum(1 for o in priced if (o.get("entry_volume") or 0) > 0)

    return {
        "otm_pct": run.get("otm_pct"),
        "month_offset": run.get("month_offset"),

        "total_months": total_months,
        "priced_months": len(priced),
        "coverage_pct": (100.0 * len(priced) / total_months) if total_months else 0.0,
        # What the filter turned away, so a shrunken run explains itself rather than
        # looking like the data thinned out.
        "skipped_low_yield": int(run.get("skipped_low_yield") or 0),
        "skipped_no_baseline": int(run.get("skipped_no_baseline") or 0),
        "priceable_months": int(run.get("priced_months") or 0),
        # How many priced months rest on a strike that actually traded on the entry
        # day. A low figure means the headline return was largely assembled from
        # carried settlement marks, at premiums nobody could have sold at.
        "traded_months": traded,
        "traded_pct": (100.0 * traded / len(priced)) if priced else 0.0,

        # Two rates, deliberately. The engine's held_rate counts unpriced months
        # using a strike that was never tradeable; strike_held_pct counts only real
        # trades. They differ whenever coverage is below 100%.
        "engine_held_rate_pct": 100.0 * float(run.get("held_rate") or 0.0),
        "strike_held_pct": (100.0 * held / len(priced)) if priced else 0.0,

        "cumulative_pct": curve[-1]["cumulative_pct"] if curve else 0.0,
        "max_drawdown_pct": max_drawdown_pct(curve),

        "profitable_months": len(wins),
        "losing_months": len(losses),
        "avg_win_pct": (sum(wins) / len(wins)) if wins else 0.0,
        "avg_loss_pct": (sum(losses) / len(losses)) if losses else 0.0,
        "best_month_pct": max(profits) if profits else 0.0,
        "worst_month_pct": min(profits) if profits else 0.0,
    }


def _months_between(first: str, last: str) -> int:
    """Inclusive span in months between two "YYYY-MM" labels."""
    a = int(first[:4]) * 12 + int(first[5:7])
    b = int(last[:4]) * 12 + int(last[5:7])
    return max(1, b - a + 1)


def risk_metrics(cycles: list[dict[str, Any]], capital: float,
                 benchmark_pnl: float) -> dict[str, Any]:
    """Return, CAGR and the risk-adjusted ratios, over the cycle series.

    Returns are taken against a FIXED capital base - the holding is sized once and
    never added to - so each cycle's return is its P&L change over that same base
    rather than over a compounding balance. Annualisation uses the actual elapsed
    months, so a quarterly cycle at a longer hold is not mistaken for a monthly one.
    """
    n = len(cycles)
    if n == 0 or capital <= 0:
        return {"sample_adequate": False, "cycles": 0}

    years = _months_between(cycles[0]["entry_month"], cycles[-1]["check_month"]) / 12.0
    periods_per_year = n / years if years > 0 else 0.0

    # Per-cycle simple returns off the running P&L.
    period_returns: list[float] = []
    previous = 0.0
    for cycle in cycles:
        pnl = cycle.get("pnl") or 0.0
        period_returns.append((pnl - previous) / capital)
        previous = pnl

    end_pnl = cycles[-1].get("pnl") or 0.0
    total_return = end_pnl / capital
    benchmark_return = benchmark_pnl / capital

    def cagr(total: float) -> float:
        growth = 1.0 + total
        if years <= 0 or growth <= 0:
            return float("nan")   # a total wipeout has no meaningful annual rate
        return growth ** (1.0 / years) - 1.0

    mean = statistics.fmean(period_returns)
    # Sample stdev needs two points; one cycle has no dispersion to measure.
    stdev = statistics.stdev(period_returns) if n > 1 else 0.0
    downside = [min(0.0, r) for r in period_returns]
    downside_dev = math.sqrt(statistics.fmean([d * d for d in downside]))

    scale = math.sqrt(periods_per_year) if periods_per_year > 0 else 0.0
    sharpe = (mean / stdev) * scale if stdev > 0 else float("nan")
    sortino = (mean / downside_dev) * scale if downside_dev > 0 else float("nan")

    return {
        "cycles": n,
        "years": years,
        "capital": capital,
        "total_return_pct": 100.0 * total_return,
        "benchmark_return_pct": 100.0 * benchmark_return,
        "cagr_pct": 100.0 * cagr(total_return),
        "benchmark_cagr_pct": 100.0 * cagr(benchmark_return),
        # Excess return is risk-free-rate-free on purpose: no rate is wired in yet,
        # so these are raw return-per-unit-risk, not true Sharpe/Sortino against a
        # T-bill. Labelled as such wherever they are shown.
        "sharpe": sharpe,
        "sortino": sortino,
        "stdev_pct": 100.0 * stdev,
        "periods_per_year": periods_per_year,
        # The video that prompted these judged a strategy over 3,500 trades. Ours
        # has dozens. The ratios are computed either way, and flagged when the
        # sample cannot support them.
        "sample_adequate": n >= MIN_CYCLES_FOR_RATIOS,
        "min_cycles_for_ratios": MIN_CYCLES_FOR_RATIOS,
    }


def position_summary(run: dict[str, Any]) -> dict[str, Any]:
    """The share leg: currency P&L against simply holding the stock.

    Reported separately from the percentage figures because it answers a different
    question. `cumulative_pct` is the option leg alone and says nothing about
    whether writing calls beat owning the shares; this does.
    """
    pos: dict[str, Any] = run.get("position") or {}
    cycles: list[dict[str, Any]] = pos.get("cycles") or []

    total_months = int(run.get("total_months") or 0)
    skipped_lot = int(pos.get("skipped_unknown_lot") or 0)
    skipped_small = int(pos.get("skipped_under_one_lot") or 0)

    premium = sum(c.get("premium_collected") or 0.0 for c in cycles)
    run_pnl, peak, drawdown = 0.0, 0.0, 0.0
    for c in cycles:
        run_pnl = c.get("pnl") or 0.0
        peak = max(peak, run_pnl)
        drawdown = max(drawdown, peak - run_pnl)

    return {
        "cycles_run": int(pos.get("cycles_run") or 0),
        "assignments": int(pos.get("assignments") or 0),
        "skipped_unknown_lot": skipped_lot,
        "skipped_under_one_lot": skipped_small,
        "cycles_on_override": int(pos.get("cycles_on_override") or 0),
        # What share of the tested period the currency figures actually cover. The
        # percentage metrics above span more months, so the two are not comparable
        # unless this is 100.
        "covered_months": len(cycles),
        "coverage_pct": (100.0 * len(cycles) / total_months) if total_months else 0.0,
        "premium_collected": premium,
        "end_pnl": float(pos.get("end_pnl") or 0.0),
        "benchmark_pnl": float(pos.get("benchmark_pnl") or 0.0),
        # A difference, not a ratio: both sides can be negative, and 0.93x of a loss
        # is an outperformance that a ratio would report as underperformance.
        "excess_pnl": float(pos.get("excess_pnl") or 0.0),
        "max_drawdown": drawdown,
        "shares_held_at_end": int(pos.get("shares_held_at_end") or 0),
        "shares": int(cycles[0].get("shares") or 0) if cycles else 0,
        "first_month": cycles[0].get("entry_month") if cycles else None,
        "last_month": cycles[-1].get("check_month") if cycles else None,
        "risk": risk_metrics(
            cycles,
            capital=(float(cycles[0].get("shares") or 0) * float(cycles[0].get("entry_spot") or 0.0))
            if cycles else 0.0,
            benchmark_pnl=float(pos.get("benchmark_pnl") or 0.0),
        ),
    }


def detail(run: dict[str, Any]) -> dict[str, Any]:
    """Summary plus the per-month rows and the curve, for the single-run view."""
    return {
        "summary": summarize(run),
        "equity_curve": equity_curve(run.get("outcomes") or []),
        "outcomes": run.get("outcomes") or [],
        "position": position_summary(run),
        "cycles": (run.get("position") or {}).get("cycles") or [],
        "diagnostics": diagnose.diagnose(run.get("outcomes") or []),
    }
