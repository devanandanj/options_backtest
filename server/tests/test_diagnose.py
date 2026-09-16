"""Losing-cycle diagnostics: the rank statistic and the guards around it."""

from __future__ import annotations

from server import diagnose


def cycle(profit, *, premium=1.0, entry=100.0, strike=110.0, exit_spot=105.0,
          volume=500, vol=25.0, momentum=0.0, range_pos=50.0, dte=25,
          complete=True, priced=True, entered=None):
    return {
        "priced": priced,
        # With no yield filter an entered cycle is any priceable one.
        "entered": priced if entered is None else entered,
        "profit_pct": profit,
        "premium": premium,
        "entry_price": entry,
        "rounded_strike": strike,
        "check_price": exit_spot,
        "entry_volume": volume,
        "context": {
            "trailing_vol_pct": vol,
            "momentum_pct": momentum,
            "range_pos_pct": range_pos,
            "days_to_expiry": dte,
            "complete": complete,
        },
    }


# ── Cliff's delta ────────────────────────────────────────────────

def test_delta_is_one_when_every_loser_outranks_every_winner():
    assert diagnose.cliffs_delta([5, 6, 7], [1, 2, 3]) == 1.0


def test_delta_is_minus_one_when_reversed():
    assert diagnose.cliffs_delta([1, 2, 3], [5, 6, 7]) == -1.0


def test_delta_is_zero_for_identical_groups():
    assert diagnose.cliffs_delta([1, 2, 3], [1, 2, 3]) == 0.0


def test_delta_is_zero_when_a_group_is_empty():
    assert diagnose.cliffs_delta([], [1, 2]) == 0.0


def test_delta_ignores_outlier_magnitude():
    # A rank statistic, not a difference of means: one absurd value must not
    # manufacture separation the ordering does not support.
    modest = diagnose.cliffs_delta([4, 5, 6], [1, 2, 3])
    extreme = diagnose.cliffs_delta([4, 5, 10_000], [1, 2, 3])
    assert modest == extreme == 1.0


# ── Grouping and guards ──────────────────────────────────────────

def test_break_even_counts_as_a_loss():
    # Zero P&L is not a win; a cycle that collected nothing still tied up capital.
    r = diagnose.diagnose([cycle(0.0), cycle(1.0)])
    assert r["losing_cycles"] == 1
    assert r["winning_cycles"] == 1


def test_declined_cycles_are_excluded_from_the_comparison():
    # A cycle the filter turned away has no outcome, so it belongs in neither group.
    r = diagnose.diagnose([cycle(1.0), cycle(-1.0, entered=False)])
    assert r["priced_cycles"] == 1
    assert r["losing_cycles"] == 0


def test_unpriced_cycles_are_excluded_entirely():
    r = diagnose.diagnose([cycle(1.0), cycle(-1.0, priced=False)])
    assert r["priced_cycles"] == 1
    assert r["losing_cycles"] == 0


def test_small_samples_are_flagged_inadequate():
    outcomes = [cycle(1.0) for _ in range(40)] + [cycle(-1.0) for _ in range(3)]
    r = diagnose.diagnose(outcomes)
    assert r["sample_adequate"] is False
    # Findings are still computed - they are just not to be believed.
    assert r["findings"]


def test_adequacy_needs_both_groups_not_just_a_big_total():
    outcomes = [cycle(1.0) for _ in range(200)] + [cycle(-1.0) for _ in range(2)]
    assert diagnose.diagnose(outcomes)["sample_adequate"] is False


def test_incomplete_context_drops_only_the_features_that_need_it():
    # A cycle without enough history still contributes its premium yield; it just
    # cannot contribute a volatility or range reading.
    outcomes = ([cycle(1.0, complete=False, premium=1.0) for _ in range(12)]
                + [cycle(-1.0, complete=False, premium=3.0) for _ in range(12)])
    r = diagnose.diagnose(outcomes)
    names = {f["feature"]: f for f in r["findings"]}
    assert "premium_yield_pct" in names
    assert "trailing_vol_pct" not in names
    assert names["premium_yield_pct"]["losers_n"] == 12


# ── Findings ─────────────────────────────────────────────────────

def test_a_separating_feature_is_found_and_directed():
    outcomes = ([cycle(1.0, vol=10.0) for _ in range(12)]
                + [cycle(-1.0, vol=60.0) for _ in range(12)])
    r = diagnose.diagnose(outcomes)
    top = r["findings"][0]
    assert top["feature"] == "trailing_vol_pct"
    assert top["delta"] == 1.0
    assert top["direction"] == "higher in losers"
    assert top["magnitude"] == "large"


def test_findings_are_ranked_by_strength_not_sign():
    # A strong negative separation must outrank a weak positive one.
    outcomes = ([cycle(1.0, vol=60.0, momentum=1.0) for _ in range(12)]
                + [cycle(-1.0, vol=10.0, momentum=0.9) for _ in range(12)])
    r = diagnose.diagnose(outcomes)
    assert r["findings"][0]["feature"] == "trailing_vol_pct"
    assert r["findings"][0]["delta"] == -1.0


def test_outcome_features_are_reported_apart_from_findings():
    # The realised move separates perfectly by construction, and must never appear
    # among the actionable findings - it is not knowable at entry.
    outcomes = ([cycle(1.0, exit_spot=90.0) for _ in range(12)]
                + [cycle(-1.0, exit_spot=140.0) for _ in range(12)])
    r = diagnose.diagnose(outcomes)
    assert all(f["feature"] not in diagnose.OUTCOME_FEATURES for f in r["findings"])
    assert {f["feature"] for f in r["outcome_context"]} == set(diagnose.OUTCOME_FEATURES)


def test_features_tested_is_reported_for_multiple_comparisons():
    outcomes = [cycle(1.0) for _ in range(12)] + [cycle(-1.0) for _ in range(12)]
    r = diagnose.diagnose(outcomes)
    assert r["features_tested"] == len(r["findings"]) > 1


def test_a_custom_loss_test_can_redefine_the_split():
    # Losing in rupees and losing versus a benchmark are different questions.
    outcomes = [cycle(1.0), cycle(5.0)]
    r = diagnose.diagnose(outcomes, is_loss=lambda o: o["profit_pct"] < 3.0)
    assert r["losing_cycles"] == 1
