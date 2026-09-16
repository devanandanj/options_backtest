"""Locating and invoking the backtest_cli engine binary.

The engine is a separate process that emits one JSON document on stdout. Anything
else on stdout means a stray print crept into the C++ side, so unparseable output
is treated as a distinct failure rather than a generic error.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent

# Searched in order. The env var wins so a non-standard build tree can be pointed at
# without editing code.
_ENGINE_ENV_VAR = "OPTIONS_BACKTEST_ENGINE"
_ENGINE_CANDIDATES = (
    "cmake-build-debug/bin/backtest_cli.exe",
    "cmake-build-debug/bin/backtest_cli",
    "build/bin/backtest_cli.exe",
    "build/bin/backtest_cli",
)

DEFAULT_TIMEOUT_SECONDS = 120.0
_CACHE_MAX_ENTRIES = 32


class EngineError(RuntimeError):
    """Base for every way an engine invocation can fail."""


class EngineNotFound(EngineError):
    pass


class EngineFailed(EngineError):
    """Engine ran but exited non-zero."""

    def __init__(self, message: str, exit_code: int, stderr_tail: str) -> None:
        super().__init__(message)
        self.exit_code = exit_code
        self.stderr_tail = stderr_tail


class EngineBadOutput(EngineError):
    """Engine exited zero but stdout was not a single JSON document."""

    def __init__(self, message: str, stdout_head: str) -> None:
        super().__init__(message)
        self.stdout_head = stdout_head


class EngineTimeout(EngineError):
    pass


def find_engine() -> Path | None:
    """Resolve the engine binary, or None if no candidate exists."""
    override = os.environ.get(_ENGINE_ENV_VAR)
    if override:
        candidate = Path(override).expanduser()
        if not candidate.is_absolute():
            candidate = (REPO_ROOT / candidate).resolve()
        return candidate if candidate.is_file() else None

    for rel in _ENGINE_CANDIDATES:
        candidate = REPO_ROOT / rel
        if candidate.is_file():
            return candidate

    on_path = shutil.which("backtest_cli")
    return Path(on_path) if on_path else None


def engine_status() -> dict[str, Any]:
    """Diagnostics for /api/health — the first thing to check when nothing works."""
    engine = find_engine()
    return {
        "engine_path": str(engine) if engine else None,
        "engine_found": engine is not None,
        "repo_root": str(REPO_ROOT),
        "env_override": os.environ.get(_ENGINE_ENV_VAR),
        "searched": [str(REPO_ROOT / rel) for rel in _ENGINE_CANDIDATES],
        "build_hint": "cmake --build cmake-build-debug --target backtest_cli",
    }


@dataclass(frozen=True)
class RunRequest:
    equity_path: Path
    chain_path: Path | None
    otm_pcts: tuple[float, ...]
    month_offsets: tuple[int, ...]
    min_entry_volume: int = 0
    lots: int = 1
    lot_size_override: int = 0
    rebuy_after_assignment: bool = True
    yield_percentile: float = 0.0
    yield_lookback: int = 12

    def argv(self, engine: Path) -> list[str]:
        args = [str(engine), "--equity", str(self.equity_path)]
        if self.chain_path is not None:
            args += ["--chain", str(self.chain_path)]
        args += ["--otm", ",".join(repr(float(p)) for p in self.otm_pcts)]
        args += ["--offset", ",".join(str(int(o)) for o in self.month_offsets)]
        if self.min_entry_volume:
            args += ["--min-volume", str(int(self.min_entry_volume))]
        if self.lots != 1:
            args += ["--lots", str(int(self.lots))]
        if self.lot_size_override:
            args += ["--lot-size", str(int(self.lot_size_override))]
        if not self.rebuy_after_assignment:
            args += ["--no-rebuy"]
        if self.yield_percentile:
            args += ["--yield-pct", repr(float(self.yield_percentile))]
            args += ["--yield-lookback", str(int(self.yield_lookback))]
        return args

    def cache_key(self) -> tuple:
        """Includes mtimes so edited data — or a rebuilt engine — invalidates the entry."""

        def stamp(p: Path | None) -> tuple:
            if p is None:
                return ()
            try:
                st = p.stat()
                return (str(p), st.st_mtime_ns, st.st_size)
            except OSError:
                return (str(p), None, None)

        # The engine binary is part of the key: rebuilding it changes the numbers,
        # and without this a stale result would survive the rebuild.
        return (stamp(self.equity_path), stamp(self.chain_path), stamp(find_engine()),
                self.otm_pcts, self.month_offsets, self.min_entry_volume,
                self.lots, self.lot_size_override, self.rebuy_after_assignment,
                self.yield_percentile, self.yield_lookback)


# Raw engine JSON only. Metrics are derived outside the cache so they can be
# changed without a stale-cache footgun.
_cache: OrderedDict[tuple, dict[str, Any]] = OrderedDict()


def clear_cache() -> None:
    _cache.clear()


async def run_engine(request: RunRequest,
                     timeout: float = DEFAULT_TIMEOUT_SECONDS) -> dict[str, Any]:
    """Invoke the engine and return its parsed JSON document."""
    key = request.cache_key()
    if key in _cache:
        _cache.move_to_end(key)
        return _cache[key]

    engine = find_engine()
    if engine is None:
        raise EngineNotFound(
            "backtest_cli not found. Build it with: "
            "cmake --build cmake-build-debug --target backtest_cli"
        )

    argv = request.argv(engine)
    try:
        proc = await asyncio.create_subprocess_exec(
            *argv,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=str(REPO_ROOT),
        )
    except OSError as exc:
        raise EngineNotFound(f"could not start engine at {engine}: {exc}") from exc

    try:
        stdout_b, stderr_b = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        raise EngineTimeout(f"engine exceeded {timeout:.0f}s and was killed") from None

    stdout = stdout_b.decode("utf-8", errors="replace")
    stderr = stderr_b.decode("utf-8", errors="replace")

    if proc.returncode != 0:
        # The CLI emits a JSON error body on stdout even when failing; prefer its
        # message over the raw stderr dump when it parses.
        message = f"engine exited {proc.returncode}"
        try:
            payload = json.loads(stdout)
            if isinstance(payload, dict) and "error" in payload:
                message = str(payload["error"])
        except json.JSONDecodeError:
            pass
        raise EngineFailed(message, exit_code=proc.returncode, stderr_tail=stderr[-4000:])

    try:
        document = json.loads(stdout)
    except json.JSONDecodeError as exc:
        raise EngineBadOutput(
            f"engine stdout was not valid JSON ({exc}); "
            "a stray print in the C++ layer is the usual cause",
            stdout_head=stdout[:2000],
        ) from exc

    _cache[key] = document
    _cache.move_to_end(key)
    while len(_cache) > _CACHE_MAX_ENTRIES:
        _cache.popitem(last=False)

    return document
