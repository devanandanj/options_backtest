"""What losing cycles have in common.

The question this answers is "what could I have seen at entry that warned me",
so features are split in two and only one half is allowed to produce findings:

  ENTRY    known before the trade was placed. Actionable.
  OUTCOME  measured over the cycle. Describes the loss; cannot predict it.
           "Losing months are the ones where the stock rallied" is true and
           useless, so these are reported for understanding and never ranked.

Separation is measured with Cliff's delta, a rank statistic: the probability
that a random losing cycle scores above a random winning one, minus the reverse.
It is used instead of a difference of means because these samples are small,
skewed, and contain outliers that would drag a mean around. Ranges from -1 to +1;
0 means the two groups are indistinguishable on that feature.

No p-values. With dozens of cycles and a handful of features, a p-value would
imply a level of confidence the sample cannot support, and testing several
features at once inflates any such claim further. Effect size and sample count
are reported instead, and the caller is told plainly when n is too small to
conclude anything.
"""

from __future__ import annotations

import statistics
from typing import Any, Callable

# Cliff's delta interpretation bands, from Romano et al. Kept explicit so the
# wording attached to a finding is traceable to a number rather than to taste.
NEGLIGIBLE, SMALL, MEDIUM = 0.147, 0.33, 0.474

# Under this many cycles in either group, nothing here is worth reading as a
# pattern. Two dozen a side is already thin; below ten it is anecdote.
MIN_GROUP = 10


def _safe_div(a: float, b: float) -> float | None:
    return (a / b) if b else None


def _feature_values(outcome: dict[str, Any]) -> dict[str, float | None]:
    """Every feature for one cycle, entry-time and outcome alike."""
    ctx = outcome.get("context") or {}
    entry = outcome.get("entry_price") or 0.0
    strike = outcome.get("rounded_strike") or 0.0
    exit_spot = outcome.get("check_price") or 0.0
    complete = bool(ctx.get("complete"))

    premium_yield = _safe_div(outcome.get("premium") or 0.0, entry)
    otm = _safe_div(strike, entry)
    move = _safe_div(exit_spot, entry)
    breach = _safe_div(exit_spot, strike)

    return {
        # ── entry-time ──────────────────────────────────────────────
        "premium_yield_pct": premium_yield * 100 if premium_yield is not None else None,
        "otm_actual_pct": (otm - 1) * 100 if otm is not None else None,
        "entry_volume": float(outcome.get("entry_volume") or 0),
        "days_to_expiry": float(ctx["days_to_expiry"]) if ctx.get("days_to_expiry") else None,
        "trailing_vol_pct": float(ctx["trailing_vol_pct"]) if complete else None,
        "momentum_pct": float(ctx["momentum_pct"]) if complete else None,
        "range_pos_pct": float(ctx["range_pos_pct"]) if complete else None,
        # ── outcome, never ranked ───────────────────────────────────
        "realised_move_pct": (move - 1) * 100 if move is not None else None,
        "breach_pct": (breach - 1) * 100 if breach is not None else None,
    }


ENTRY_FEATURES: dict[str, str] = {
    "premium_yield_pct": "Premium yield (% of spot)",
    "otm_actual_pct": "Strike distance (% OTM)",
    "trailing_vol_pct": "Trailing volatility (annualised %)",
    "momentum_pct": "Run-in momentum (%)",
    "range_pos_pct": "Position in trailing range (0-100)",
    "days_to_expiry": "Days to expiry at entry",
    "entry_volume": "Entry-day volume (contracts)",
}

OUTCOME_FEATURES: dict[str, str] = {
    "realised_move_pct": "Underlying move over the cycle (%)",
    "breach_pct": "Finish vs strike (%)",
}


def cliffs_delta(losers: list[float], winners: list[float]) -> float:
    """P(loser > winner) - P(loser < winner), over all pairs."""
    if not losers or not winners:
        return 0.0
    above = below = 0
    for a in losers:
        for b in winners:
            if a > b:
                above += 1
            elif a < b:
                below += 1
    return (above - below) / (len(losers) * len(winners))


def _magnitude(delta: float) -> str:
    size = abs(delta)
    if size < NEGLIGIBLE:
        return "negligible"
    if size < SMALL:
        return "small"
    if size < MEDIUM:
        return "medium"
    return "large"


def _describe(name: str, losers: list[float], winners: list[float]) -> dict[str, Any]:
    delta = cliffs_delta(losers, winners)
    return {
        "feature": name,
        "label": ENTRY_FEATURES.get(name) or OUTCOME_FEATURES.get(name) or name,
        "losers_n": len(losers),
        "winners_n": len(winners),
        "losers_median": statistics.median(losers) if losers else None,
        "winners_median": statistics.median(winners) if winners else None,
        "delta": delta,
        "magnitude": _magnitude(delta),
        # Which way it leans, in words, so a negative delta is not misread.
        "direction": ("higher in losers" if delta > 0 else
                      "lower in losers" if delta < 0 else "no difference"),
    }


def diagnose(outcomes: list[dict[str, Any]],
             is_loss: Callable[[dict[str, Any]], bool] | None = None) -> dict[str, Any]:
    """Compare entry conditions between profitable and losing cycles."""
    # Entered, not priced: a cycle the yield filter declined produced no trade and
    # therefore no win or loss to attribute anything to.
    priced = [o for o in outcomes if o.get("entered")]
    loss_test = is_loss or (lambda o: (o.get("profit_pct") or 0.0) <= 0.0)

    losers = [o for o in priced if loss_test(o)]
    winners = [o for o in priced if not loss_test(o)]

    def column(group: list[dict[str, Any]], name: str) -> list[float]:
        values = [_feature_values(o).get(name) for o in group]
        return [v for v in values if v is not None]

    findings = []
    for name in ENTRY_FEATURES:
        lo, wi = column(losers, name), column(winners, name)
        if not lo or not wi:
            continue
        findings.append(_describe(name, lo, wi))
    # Strongest separation first; sign is direction, not strength.
    findings.sort(key=lambda f: abs(f["delta"]), reverse=True)

    context = []
    for name in OUTCOME_FEATURES:
        lo, wi = column(losers, name), column(winners, name)
        if lo and wi:
            context.append(_describe(name, lo, wi))

    usable = min(len(losers), len(winners))
    return {
        "priced_cycles": len(priced),
        "losing_cycles": len(losers),
        "winning_cycles": len(winners),
        "min_group": MIN_GROUP,
        # The single most important field here. Every finding below is noise until
        # this is true, and even then it is a hypothesis rather than a rule.
        "sample_adequate": usable >= MIN_GROUP,
        "features_tested": len(findings),
        "findings": findings,
        "outcome_context": context,
    }
