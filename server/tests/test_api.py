"""Endpoint tests, including every way the engine subprocess can fail."""

import json

import pytest
from fastapi.testclient import TestClient

from server import engine
from server.main import app

client = TestClient(app)


@pytest.fixture(autouse=True)
def clear_engine_cache():
    engine.clear_cache()
    yield
    engine.clear_cache()


def _symbol():
    datasets = client.get("/api/datasets").json()["datasets"]
    if not datasets:
        pytest.skip("no datasets in data/sample/")
    return datasets[0]["symbol"]


def fake_document(otm=0.10, offset=1):
    return {
        "schema_version": 1,
        "params": {}, "meta": {"equity_rows": 10, "chain_rows": 20, "months": 3},
        "runs": [{
            "otm_pct": otm, "month_offset": offset,
            "total_months": 2, "held_months": 1, "held_rate": 0.5, "priced_months": 1,
            "outcomes": [
                {"entry_month": "2024-01", "entry_price": 100.0, "strike": 110.0,
                 "check_month": "2024-02", "check_price": 105.0, "held": True,
                 "rounded_strike": 110.0, "premium": 2.0, "profit_pct": 2.0, "priced": True,
                 "entered": True},
                {"entry_month": "2024-02", "entry_price": 100.0, "strike": 110.0,
                 "check_month": "2024-03", "check_price": 120.0, "held": False,
                 "rounded_strike": 0.0, "premium": 0.0, "profit_pct": 0.0, "priced": False,
                 "entered": False},
            ],
        }],
    }


# ── Health and discovery ─────────────────────────────────────────

def test_health_reports_engine_and_dataset_state():
    body = client.get("/api/health").json()
    assert "engine_found" in body
    assert "build_hint" in body
    assert isinstance(body["datasets_found"], int)


def test_datasets_pairs_underlying_with_chain():
    for d in client.get("/api/datasets").json()["datasets"]:
        assert d["symbol"]
        assert d["equity_file"].endswith(".csv")
        if d["has_chain"]:
            assert d["chain_file"].endswith(".csv")


# ── Request validation ───────────────────────────────────────────

def test_unknown_symbol_is_rejected_with_the_available_list():
    r = client.post("/api/backtest", json={"symbol": "definitely-not-real",
                                           "otm_pct": 0.1, "month_offset": 1})
    assert r.status_code == 404
    assert "available" in r.json()["detail"]


@pytest.mark.parametrize("payload", [
    {"symbol": "x", "otm_pct": 0, "month_offset": 1},      # otm must be > 0
    {"symbol": "x", "otm_pct": 0.1, "month_offset": -1},   # offset must be >= 0
    {"symbol": "x", "otm_pct": 99, "month_offset": 1},     # otm above the cap
])
def test_out_of_range_parameters_are_rejected_before_the_engine_runs(payload):
    assert client.post("/api/backtest", json=payload).status_code == 422


# ── Engine failure modes ─────────────────────────────────────────

def _patch_engine(monkeypatch, exc):
    async def boom(*_args, **_kwargs):
        raise exc
    monkeypatch.setattr("server.main.run_engine", boom)


def test_missing_engine_binary_returns_503_with_a_build_hint(monkeypatch):
    _patch_engine(monkeypatch, engine.EngineNotFound("backtest_cli not found"))
    r = client.post("/api/backtest", json={"symbol": _symbol(),
                                           "otm_pct": 0.1, "month_offset": 1})
    assert r.status_code == 503
    assert "hint" in r.json()["detail"]


def test_engine_nonzero_exit_returns_500_with_stderr(monkeypatch):
    _patch_engine(monkeypatch, engine.EngineFailed("engine exited 1", 1, "boom on stderr"))
    r = client.post("/api/backtest", json={"symbol": _symbol(),
                                           "otm_pct": 0.1, "month_offset": 1})
    assert r.status_code == 500
    detail = r.json()["detail"]
    assert detail["exit_code"] == 1
    assert "boom on stderr" in detail["stderr_tail"]


def test_unparseable_stdout_returns_502_not_500(monkeypatch):
    # Distinct code on purpose: this means a stray print reached stdout, which is
    # a different problem from the engine failing honestly.
    _patch_engine(monkeypatch, engine.EngineBadOutput("not JSON", "Loading CSV...\n{"))
    r = client.post("/api/backtest", json={"symbol": _symbol(),
                                           "otm_pct": 0.1, "month_offset": 1})
    assert r.status_code == 502
    assert "Loading CSV" in r.json()["detail"]["stdout_head"]


def test_engine_timeout_returns_504(monkeypatch):
    _patch_engine(monkeypatch, engine.EngineTimeout("engine exceeded 120s"))
    r = client.post("/api/backtest", json={"symbol": _symbol(),
                                           "otm_pct": 0.1, "month_offset": 1})
    assert r.status_code == 504


# ── Success paths against a stubbed engine ───────────────────────

def test_backtest_shape_and_unpriced_exclusion(monkeypatch):
    async def ok(*_args, **_kwargs):
        return fake_document()
    monkeypatch.setattr("server.main.run_engine", ok)

    body = client.post("/api/backtest", json={"symbol": _symbol(),
                                              "otm_pct": 0.1, "month_offset": 1}).json()
    s = body["summary"]
    assert s["total_months"] == 2
    assert s["priced_months"] == 1
    assert s["coverage_pct"] == 50.0
    # Both outcomes are returned for display, but only the priced one is charted.
    assert len(body["outcomes"]) == 2
    assert len(body["equity_curve"]) == 1


def test_sweep_returns_one_cell_per_combination_without_outcomes(monkeypatch):
    async def ok(*_args, **_kwargs):
        doc = fake_document()
        base = doc["runs"][0]
        doc["runs"] = []
        for otm in (0.05, 0.10):
            for off in (0, 1):
                run = json.loads(json.dumps(base))
                run["otm_pct"], run["month_offset"] = otm, off
                doc["runs"].append(run)
        return doc
    monkeypatch.setattr("server.main.run_engine", ok)

    body = client.post("/api/sweep", json={"symbol": _symbol(),
                                           "otm_pcts": [0.05, 0.10],
                                           "month_offsets": [0, 1]}).json()
    assert len(body["cells"]) == 4
    # Cells carry summaries only; shipping outcomes per cell would bloat the payload.
    assert all("outcomes" not in cell for cell in body["cells"])
    assert "outcomes" in body["best"]


# ── Real engine ──────────────────────────────────────────────────

@pytest.mark.skipif(engine.find_engine() is None, reason="backtest_cli not built")
def test_real_engine_reproduces_the_known_result():
    # Named, not datasets[0]. This baseline belongs to IDFCFIRSTB specifically, and
    # picking the first discovered symbol silently re-pointed it at BANKNIFTY the
    # moment a second dataset was added.
    # Chain CSVs are not committed - they are large and reproducible from
    # tools/download_bhavcopies.py - so a fresh clone legitimately has no chain to
    # price against. Skip rather than fail: the absence is expected, not a defect.
    found = {d["symbol"]: d for d in client.get("/api/datasets").json()["datasets"]}
    dataset = found.get("idfcfirstb")
    if dataset is None:
        pytest.skip("idfcfirstb sample not present")
    if not dataset["has_chain"]:
        pytest.skip("no option chain on disk - run tools/download_bhavcopies.py")

    body = client.post("/api/backtest", json={"symbol": "idfcfirstb",
                                              "otm_pct": 0.10, "month_offset": 2})
    assert body.status_code == 200, body.text
    s = body.json()["summary"]
    # 0.672727 (37/55) again, but arrived at honestly. Reaching it took fixing the
    # equity feed's dates (IST midnight was being read as UTC, shifting every bar a
    # day earlier), restoring a month the feed had dropped, and downloading every
    # expiry rather than front-month contracts only - before which offset 2 priced
    # a different contract than it sold. priced_months is pinned alongside because
    # the headline rate counts unpriced months too and would hide a coverage drop.
    assert s["total_months"] == 55
    assert round(s["engine_held_rate_pct"], 4) == 67.2727
    assert s["priced_months"] == 53
