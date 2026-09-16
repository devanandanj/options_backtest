"""Discovery of available datasets under data/sample/.

The UI picks a symbol from this list and the server maps it back to concrete
paths. Clients never supply paths directly, so there is no route from a request
body into a subprocess argument.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .engine import REPO_ROOT

DATA_DIR = REPO_ROOT / "data" / "sample"

# Produced by tools/: {symbol}_underlying_{startyear}_{endyear}.csv
#                 and {symbol}_{ce|pe}_{startyear}_{endyear}.csv
_UNDERLYING_RE = re.compile(r"^(?P<symbol>.+?)_underlying_(?P<start>\d{4})_(?P<end>\d{4})\.csv$",
                            re.IGNORECASE)
_CHAIN_RE = re.compile(r"^(?P<symbol>.+?)_(?P<right>ce|pe)_(?P<start>\d{4})_(?P<end>\d{4})\.csv$",
                       re.IGNORECASE)


@dataclass(frozen=True)
class Dataset:
    symbol: str
    equity_path: Path
    chain_path: Path | None
    start_year: int
    end_year: int

    def to_dict(self) -> dict:
        return {
            "symbol": self.symbol,
            "equity_file": self.equity_path.name,
            "chain_file": self.chain_path.name if self.chain_path else None,
            "start_year": self.start_year,
            "end_year": self.end_year,
            "has_chain": self.chain_path is not None,
        }


def list_datasets(data_dir: Path | None = None) -> list[Dataset]:
    """Pair each underlying CSV with a chain CSV for the same symbol.

    An underlying file with no matching chain is still listed — it can be
    backtested, just with every month unpriced.
    """
    directory = data_dir or DATA_DIR
    if not directory.is_dir():
        return []

    chains: dict[str, Path] = {}
    for path in sorted(directory.glob("*.csv")):
        match = _CHAIN_RE.match(path.name)
        if match:
            chains.setdefault(match.group("symbol").lower(), path)

    datasets: list[Dataset] = []
    for path in sorted(directory.glob("*.csv")):
        match = _UNDERLYING_RE.match(path.name)
        if not match:
            continue
        symbol = match.group("symbol").lower()
        datasets.append(Dataset(
            symbol=symbol,
            equity_path=path,
            chain_path=chains.get(symbol),
            start_year=int(match.group("start")),
            end_year=int(match.group("end")),
        ))
    return datasets


def resolve(symbol: str, data_dir: Path | None = None) -> Dataset:
    """Look up a dataset by symbol. Raises KeyError if it isn't one we listed."""
    wanted = symbol.strip().lower()
    for dataset in list_datasets(data_dir):
        if dataset.symbol == wanted:
            return dataset
    raise KeyError(symbol)
