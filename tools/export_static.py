"""Freeze the web UI into a static site.

Vercel runs Linux serverless functions; the engine is a compiled binary that
shells out and spends about a second re-parsing 95,000 chain rows per call. That
combination does not belong behind a serverless request. So instead of deploying
the engine, this runs it locally across a grid of parameters and writes each
result to a file the frontend can fetch directly.

The frontend serves both modes from one codebase: if `api/manifest.json` exists
it switches to static mode and reads precomputed files, otherwise it POSTs to the
live FastAPI server as before. Local development is unchanged.

The honest cost is that a static build can only answer questions it was built to
answer. Rather than let the UI accept a parameter and fail, static mode turns the
inputs into dropdowns listing exactly what was exported.

Run from the repository root, with the engine built:

    python tools/export_static.py
"""

from __future__ import annotations

import asyncio
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

# Same constraint the server documents: on Windows only the Proactor loop can
# spawn a subprocess, and asyncio.run picks the default policy. Set before any
# loop exists rather than discovering it as a NotImplementedError per combination.
if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsProactorEventLoopPolicy())

from server import datasets, metrics  # noqa: E402
from server.engine import RunRequest, find_engine, run_engine  # noqa: E402

OUT = REPO / "web"
STATIC_SRC = REPO / "server" / "static"

# The exported grid. Every combination here becomes a file, so this is the entire
# space the deployed site can answer. Chosen to cover the findings the README
# reports rather than to be exhaustive: the OTM ladder, all three reachable holds,
# and the two switches that changed conclusions - liquidity and premium richness.
OTM_PCTS = [0.03, 0.05, 0.10, 0.15]
MONTH_OFFSETS = [0, 1, 2]
MIN_VOLUMES = [0, 1]
YIELD_PERCENTILES = [0, 60]

# Held fixed across the export; the UI hides them in static mode rather than
# offering a control that cannot do anything.
FIXED = {
    "lots": 1,
    "lot_size_override": 0,
    "rebuy_after_assignment": True,
    "yield_lookback": 12,
}


def backtest_key(symbol: str, otm: float, offset: int, minvol: int, yield_pct: float) -> str:
    """Filesystem-safe key. The frontend rebuilds this string exactly, so the
    formatting here and in app.js must agree character for character."""
    return f"{symbol}__otm{otm * 100:.1f}__h{offset}__mv{int(minvol)}__y{yield_pct:.0f}"


def sweep_key(symbol: str, minvol: int, yield_pct: float) -> str:
    return f"{symbol}__mv{int(minvol)}__y{yield_pct:.0f}"


def _run(dataset, otm_pcts, offsets, minvol, yield_pct) -> dict:
    # run_engine is async because the server awaits it; here there is no event
    # loop to share, so each call gets its own.
    return asyncio.run(run_engine(RunRequest(
        equity_path=dataset.equity_path,
        chain_path=dataset.chain_path,
        otm_pcts=tuple(otm_pcts),
        month_offsets=tuple(offsets),
        min_entry_volume=minvol,
        lots=FIXED["lots"],
        lot_size_override=FIXED["lot_size_override"],
        rebuy_after_assignment=FIXED["rebuy_after_assignment"],
        yield_percentile=yield_pct,
        yield_lookback=FIXED["yield_lookback"],
    )))


def write_json(path: Path, payload) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Minified: these are fetched, not read by people, and the grid is large
    # enough that indentation costs real bandwidth.
    text = json.dumps(payload, separators=(",", ":"))
    path.write_text(text, encoding="utf-8")
    return len(text)


def main() -> None:
    if find_engine() is None:
        raise SystemExit("backtest_cli not found - build it first:\n"
                         "  cmake --build cmake-build-debug --target backtest_cli")

    found = datasets.list_datasets()
    if not found:
        raise SystemExit("no datasets discovered under data/sample/")

    if OUT.exists():
        shutil.rmtree(OUT)
    shutil.copytree(STATIC_SRC, OUT)
    print(f"copied frontend -> {OUT.relative_to(REPO)}")

    write_json(OUT / "api" / "datasets.json",
               {"datasets": [d.to_dict() for d in found]})

    total_bytes = 0
    combos = 0
    manifest_runs: list[str] = []
    manifest_sweeps: list[str] = []

    for dataset in found:
        for minvol in MIN_VOLUMES:
            for yield_pct in YIELD_PERCENTILES:
                # One engine call per (minvol, yield) covering the whole otm x offset
                # grid, because the CLI takes lists and parses the chain once.
                document = _run(dataset, OTM_PCTS, MONTH_OFFSETS, minvol, yield_pct)
                runs = document.get("runs") or []
                meta = document.get("meta", {})

                for run in runs:
                    key = backtest_key(dataset.symbol, run["otm_pct"],
                                       run["month_offset"], minvol, yield_pct)
                    payload = {"symbol": dataset.symbol, "meta": meta,
                               **metrics.detail(run)}
                    total_bytes += write_json(OUT / "api" / "backtest" / f"{key}.json", payload)
                    manifest_runs.append(key)
                    combos += 1

                cells = [metrics.summarize(r) for r in runs]
                best = max(range(len(cells)), key=lambda i: cells[i]["cumulative_pct"])
                skey = sweep_key(dataset.symbol, minvol, yield_pct)
                total_bytes += write_json(OUT / "api" / "sweep" / f"{skey}.json", {
                    "symbol": dataset.symbol,
                    "meta": meta,
                    "otm_pcts": OTM_PCTS,
                    "month_offsets": MONTH_OFFSETS,
                    "min_entry_volume": minvol,
                    "cells": cells,
                    "best": metrics.detail(runs[best]),
                })
                manifest_sweeps.append(skey)
                print(f"  min-volume {minvol}, yield {yield_pct:>2.0f}%"
                      f"  -> {len(runs)} runs")

    write_json(OUT / "api" / "manifest.json", {
        "generated_by": "tools/export_static.py",
        "symbols": [d.symbol for d in found],
        "otm_pcts": OTM_PCTS,
        "month_offsets": MONTH_OFFSETS,
        "min_volumes": MIN_VOLUMES,
        "yield_percentiles": YIELD_PERCENTILES,
        "fixed": FIXED,
        "runs": sorted(manifest_runs),
        "sweeps": sorted(manifest_sweeps),
    })

    print(f"\n{combos} backtests + {len(manifest_sweeps)} sweeps, "
          f"{total_bytes / 1_048_576:.1f} MB of JSON")
    print(f"static site ready in {OUT.relative_to(REPO)}/")


if __name__ == "__main__":
    main()
