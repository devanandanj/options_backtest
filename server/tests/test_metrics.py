"""Unit tests for derived statistics."""

from server import metrics


def outcome(month, profit, *, priced=True, held=True, entered=None):
    # `entered` defaults to `priced`: with no yield filter every priceable cycle is
    # opened, which is what these fixtures describe unless they say otherwise.
    return {
        "entry_month": month, "entry_price": 100.0, "strike": 110.0,
        "check_month": month, "check_price": 105.0, "held": held,
        "rounded_strike": 110.0 if priced else 0.0,
        "premium": 1.0 if priced else 0.0,
        "profit_pct": profit, "priced": priced,
        "entered": priced if entered is None else entered,
    }


def run(outcomes, *, total_months=None, held_rate=0.5):
    return {
        "otm_pct": 0.10, "month_offset": 1,
        "total_months": total_months if total_months is not None else len(outcomes),
        "held_months": sum(1 for o in outcomes if o["held"]),
        "held_rate": held_rate,
        "outcomes": outcomes,
    }


def test_equity_curve_accumulates_priced_months_only():
    curve = metrics.equity_curve([
        outcome("2024-01", 2.0),
        outcome("2024-02", 0.0, priced=False),   # must not appear at all
        outcome("2024-03", -1.0),
    ])
    assert [p["month"] for p in curve] == ["2024-01", "2024-03"]
    assert [p["cumulative_pct"] for p in curve] == [2.0, 1.0]


def test_max_drawdown_measures_from_a_mid_series_peak():
    # +5 up to a peak of 5, then down to -3: the fall from peak is 8 points,
    # which is larger than the final value's distance from zero.
    curve = metrics.equity_curve([
        outcome("2024-01", 5.0),
        outcome("2024-02", -6.0),
        outcome("2024-03", -2.0),
    ])
    assert metrics.max_drawdown_pct(curve) == 8.0


def test_max_drawdown_is_zero_for_a_monotonic_rise():
    curve = metrics.equity_curve([outcome("2024-01", 1.0), outcome("2024-02", 2.0)])
    assert metrics.max_drawdown_pct(curve) == 0.0


def test_summarize_reports_both_held_rates_when_coverage_is_partial():
    # Engine counts 4 months and says 50%; only 2 were actually tradeable and
    # both held, so the strike-held rate over real trades is 100%.
    outcomes = [
        outcome("2024-01", 1.0, held=True),
        outcome("2024-02", 1.0, held=True),
        outcome("2024-03", 0.0, priced=False, held=False),
        outcome("2024-04", 0.0, priced=False, held=False),
    ]
    s = metrics.summarize(run(outcomes, total_months=4, held_rate=0.5))

    assert s["priced_months"] == 2
    assert s["coverage_pct"] == 50.0
    assert s["engine_held_rate_pct"] == 50.0
    assert s["strike_held_pct"] == 100.0


def test_summarize_separates_average_win_from_average_loss():
    s = metrics.summarize(run([
        outcome("2024-01", 2.0),
        outcome("2024-02", 4.0),
        outcome("2024-03", -9.0),
    ]))
    assert s["profitable_months"] == 2
    assert s["losing_months"] == 1
    assert s["avg_win_pct"] == 3.0
    assert s["avg_loss_pct"] == -9.0
    assert s["best_month_pct"] == 4.0
    assert s["worst_month_pct"] == -9.0


def test_summarize_handles_all_months_unpriced_without_dividing_by_zero():
    outcomes = [outcome("2024-01", 0.0, priced=False),
                outcome("2024-02", 0.0, priced=False)]
    s = metrics.summarize(run(outcomes, total_months=2, held_rate=0.0))

    assert s["priced_months"] == 0
    assert s["coverage_pct"] == 0.0
    assert s["strike_held_pct"] == 0.0
    assert s["cumulative_pct"] == 0.0
    assert s["max_drawdown_pct"] == 0.0
    assert s["avg_win_pct"] == 0.0
    assert s["avg_loss_pct"] == 0.0


def test_summarize_handles_an_empty_run():
    s = metrics.summarize(run([], total_months=0, held_rate=0.0))
    assert s["total_months"] == 0
    assert s["coverage_pct"] == 0.0
    assert s["cumulative_pct"] == 0.0


def test_a_cycle_the_yield_filter_declined_is_not_counted_as_a_trade():
    # It was priceable, so coverage still saw it; no position was opened, so no
    # statistic about the strategy may include it.
    outcomes = [outcome("2024-01", 5.0), outcome("2024-02", -9.0, entered=False)]
    s = metrics.summarize(run(outcomes))
    assert s["priced_months"] == 1
    assert s["cumulative_pct"] == 5.0
    assert s["worst_month_pct"] == 5.0
