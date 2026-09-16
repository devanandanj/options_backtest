"""FastAPI app for the options backtester.

Localhost tool: no auth, bound to 127.0.0.1. Clients name a symbol and the server
resolves it against the datasets it discovered, so a request body can never reach
a subprocess argument.

    python -m uvicorn server.main:app --port 8000

Do not add --reload on Windows. The engine runs as a subprocess, and only the
Proactor event loop can spawn one; the reloader installs the Selector loop after
importing this module, so asyncio.create_subprocess_exec raises
NotImplementedError and every request returns 500. Setting the policy here does
not help, because uvicorn overrides it afterwards. Restart the server by hand
after editing the Python layer.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import datasets, metrics
from .engine import (
    EngineBadOutput,
    EngineFailed,
    EngineNotFound,
    EngineTimeout,
    RunRequest,
    engine_status,
    run_engine,
)

app = FastAPI(title="Options Backtester", version="0.1.0")

STATIC_DIR = Path(__file__).resolve().parent / "static"


class BacktestRequest(BaseModel):
    symbol: str
    otm_pct: float = Field(0.10, gt=0, le=2.0)
    month_offset: int = Field(1, ge=0, le=24)
    # 0 keeps strikes that were listed but never traded, whose premium is a carried
    # settlement mark rather than a price anyone could have sold at.
    min_entry_volume: int = Field(0, ge=0, le=10_000_000)
    # Share leg. lot_size_override fills only months the chain states no lot for.
    lots: int = Field(1, ge=1, le=10_000)
    lot_size_override: int = Field(0, ge=0, le=10_000_000)
    rebuy_after_assignment: bool = True
    # Premium-richness entry filter. 0 enters every priced cycle.
    yield_percentile: float = Field(0.0, ge=0.0, le=100.0)
    yield_lookback: int = Field(12, ge=1, le=120)


class SweepRequest(BaseModel):
    symbol: str
    otm_pcts: list[float] = Field(..., min_length=1, max_length=20)
    month_offsets: list[int] = Field(..., min_length=1, max_length=12)
    min_entry_volume: int = Field(0, ge=0, le=10_000_000)
    yield_percentile: float = Field(0.0, ge=0.0, le=100.0)
    yield_lookback: int = Field(12, ge=1, le=120)

    def validated(self) -> "SweepRequest":
        for p in self.otm_pcts:
            if not (0 < p <= 2.0):
                raise ValueError(f"otm_pct out of range: {p}")
        for o in self.month_offsets:
            if not (0 <= o <= 24):
                raise ValueError(f"month_offset out of range: {o}")
        return self


def _resolve_dataset(symbol: str) -> datasets.Dataset:
    try:
        return datasets.resolve(symbol)
    except KeyError:
        known = [d.symbol for d in datasets.list_datasets()]
        raise HTTPException(
            status_code=404,
            detail={"error": f"unknown symbol '{symbol}'", "available": known},
        ) from None


async def _invoke(request: RunRequest) -> dict[str, Any]:
    """Run the engine, translating each failure mode to a distinct status code."""
    try:
        return await run_engine(request)
    except EngineNotFound as exc:
        raise HTTPException(status_code=503, detail={
            "error": str(exc), "hint": engine_status()["build_hint"],
        }) from exc
    except EngineTimeout as exc:
        raise HTTPException(status_code=504, detail={"error": str(exc)}) from exc
    except EngineFailed as exc:
        raise HTTPException(status_code=500, detail={
            "error": str(exc), "exit_code": exc.exit_code, "stderr_tail": exc.stderr_tail,
        }) from exc
    except EngineBadOutput as exc:
        # 502 rather than 500: the engine claimed success but spoke gibberish.
        raise HTTPException(status_code=502, detail={
            "error": str(exc), "stdout_head": exc.stdout_head,
        }) from exc


@app.get("/api/health")
def health() -> dict[str, Any]:
    status = engine_status()
    status["datasets_found"] = len(datasets.list_datasets())
    status["data_dir"] = str(datasets.DATA_DIR)
    status["ok"] = status["engine_found"] and status["datasets_found"] > 0
    return status


@app.get("/api/datasets")
def list_datasets() -> dict[str, Any]:
    return {"datasets": [d.to_dict() for d in datasets.list_datasets()]}


@app.post("/api/backtest")
async def backtest(req: BacktestRequest) -> dict[str, Any]:
    dataset = _resolve_dataset(req.symbol)
    document = await _invoke(RunRequest(
        equity_path=dataset.equity_path,
        chain_path=dataset.chain_path,
        otm_pcts=(req.otm_pct,),
        month_offsets=(req.month_offset,),
        min_entry_volume=req.min_entry_volume,
        lots=req.lots,
        lot_size_override=req.lot_size_override,
        rebuy_after_assignment=req.rebuy_after_assignment,
        yield_percentile=req.yield_percentile,
        yield_lookback=req.yield_lookback,
    ))

    runs = document.get("runs") or []
    if not runs:
        raise HTTPException(status_code=500, detail={"error": "engine returned no runs"})

    return {
        "symbol": dataset.symbol,
        "meta": document.get("meta", {}),
        **metrics.detail(runs[0]),
    }


@app.post("/api/sweep")
async def sweep(req: SweepRequest) -> dict[str, Any]:
    try:
        req = req.validated()
    except ValueError as exc:
        raise HTTPException(status_code=422, detail={"error": str(exc)}) from exc

    dataset = _resolve_dataset(req.symbol)
    document = await _invoke(RunRequest(
        equity_path=dataset.equity_path,
        chain_path=dataset.chain_path,
        otm_pcts=tuple(req.otm_pcts),
        month_offsets=tuple(req.month_offsets),
        min_entry_volume=req.min_entry_volume,
        yield_percentile=req.yield_percentile,
        yield_lookback=req.yield_lookback,
    ))

    runs = document.get("runs") or []
    if not runs:
        raise HTTPException(status_code=500, detail={"error": "engine returned no runs"})

    # Cells carry summaries only — shipping every run's outcomes would bloat the
    # payload by roughly the grid size. The best cell gets its detail attached.
    cells = [metrics.summarize(run) for run in runs]
    best_index = max(range(len(cells)), key=lambda i: cells[i]["cumulative_pct"])

    return {
        "symbol": dataset.symbol,
        "meta": document.get("meta", {}),
        "otm_pcts": req.otm_pcts,
        "month_offsets": req.month_offsets,
        "min_entry_volume": req.min_entry_volume,
        "cells": cells,
        "best": metrics.detail(runs[best_index]),
    }


if STATIC_DIR.is_dir():
    app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")
