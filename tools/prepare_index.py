"""Make an index option chain usable by the monthly engine.

An index chain downloaded from the bhavcopy needs two things a single-stock chain
does not:

1. **A level series.** The engine needs the underlying's close to pick strikes and
   to judge expiry day. A stock has one in the equity bhavcopy; an index does not,
   so this fetches it and writes it in the same 15-column schema the loader reads,
   marked `SERIES=INDEX` rather than mislabelled as an equity.

2. **Filtering to monthly expiries.** This is the important one. BANKNIFTY quotes
   weekly contracts, so January 2022 alone carries expiries on the 6th, 13th, 20th
   and 27th. The chain index buckets by (trade month, expiry month), which assumes
   one contract per strike per bucket — true for monthly-only instruments, false
   here. Left alone, four different contracts collapse into one entry and the
   engine sells one expiry while buying back another. Measured on the raw file,
   strike 38000 in that bucket held quotes from 13.60 to 267.00 on the same day,
   because they were four different instruments.

   Keeping only the last expiry in each calendar month restores the one-per-bucket
   invariant the engine is built on, and matches a strategy that trades the monthly
   contract. Supporting weeklies properly means keying the index by expiry date
   rather than expiry month, which is a larger change.

Usage:

    python tools/prepare_index.py BANKNIFTY "NIFTY BANK"
"""

from __future__ import annotations

import csv
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = REPO / "data" / "sample"

# The schema load_equity_csv expects: 15 fields, series in column 1, close in 7.
EQUITY_COLUMNS = [
    "DATE", "SERIES", "OPEN", "HIGH", "LOW", "PREV. CLOSE", "LTP", "CLOSE",
    "VWAP", "VOLUME", "VALUE", "NO OF TRADES", "DELIVERY QTY", "DELIVERY %", "SYMBOL",
]


def monthly_expiries(chain_path: Path) -> set[str]:
    """The last expiry date in each calendar month."""
    latest: dict[str, str] = {}
    with chain_path.open(newline="") as f:
        for row in csv.DictReader(f):
            expiry = row["EXPIRY"]
            month = expiry[:7]
            if expiry > latest.get(month, ""):
                latest[month] = expiry
    return set(latest.values())


def filter_chain(chain_path: Path, keep: set[str]) -> tuple[int, int]:
    """Rewrite the chain in place, keeping only `keep` expiries.

    Written to a sibling temp file and moved into position, so an interrupted run
    cannot leave a half-filtered chain that still looks loadable.
    """
    tmp = chain_path.with_suffix(".csv.tmp")
    total = kept = 0
    with chain_path.open(newline="") as src, tmp.open("w", newline="") as dst:
        reader = csv.DictReader(src)
        writer = csv.DictWriter(dst, fieldnames=reader.fieldnames, lineterminator="\n")
        writer.writeheader()
        for row in reader:
            total += 1
            if row["EXPIRY"] in keep:
                writer.writerow(row)
                kept += 1
    tmp.replace(chain_path)
    return total, kept


def fetch_levels(index_name: str, start: str, end: str) -> list[dict[str, str]]:
    from datetime import date

    from jugaad_data.nse import index_df

    def as_date(text: str) -> date:
        return date(int(text[:4]), int(text[5:7]), int(text[8:10]))

    df = index_df(symbol=index_name, from_date=as_date(start), to_date=as_date(end))
    if df.empty:
        raise SystemExit(f"no level data returned for {index_name!r}")

    import pandas as pd

    dates = pd.to_datetime(df["HistoricalDate"]).dt.strftime("%Y-%m-%d")
    out: list[dict[str, str]] = []
    for day, o, h, low, close in zip(dates, df["OPEN"], df["HIGH"], df["LOW"], df["CLOSE"]):
        row = dict.fromkeys(EQUITY_COLUMNS, "0")
        row["DATE"] = day
        row["SERIES"] = "INDEX"
        row["OPEN"], row["HIGH"], row["LOW"], row["CLOSE"] = (
            f"{o}", f"{h}", f"{low}", f"{close}")
        row["LTP"] = f"{close}"
        row["PREV. CLOSE"] = f"{close}"
        row["SYMBOL"] = index_name.replace(" ", "")
        out.append(row)
    # index_df returns newest first; the loader and every window assume ascending.
    out.sort(key=lambda r: r["DATE"])
    return out


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    symbol, index_name = sys.argv[1], sys.argv[2]

    chain = OUT_DIR / f"{symbol.lower()}_ce_2022_2026.csv"
    if not chain.exists():
        raise SystemExit(f"chain not found: {chain}")

    keep = monthly_expiries(chain)
    weekly_dropped = Counter()
    with chain.open(newline="") as f:
        for row in csv.DictReader(f):
            if row["EXPIRY"] not in keep:
                weekly_dropped[row["EXPIRY"][:7]] += 1

    print(f"{symbol}: keeping {len(keep)} monthly expiries, "
          f"dropping weeklies from {len(weekly_dropped)} months")
    total, kept = filter_chain(chain, keep)
    print(f"  chain {total:,} -> {kept:,} rows "
          f"({chain.stat().st_size / 1_048_576:.0f} MB)")

    dates = [r["EXPIRY"][:4] for r in csv.DictReader(chain.open(newline=""))]
    start, end = f"{min(dates)}-01-01", f"{max(dates)}-12-31"

    print(f"fetching {index_name} levels {start[:4]}..{end[:4]} ...")
    levels = fetch_levels(index_name, start, end)
    out = OUT_DIR / f"{symbol.lower()}_underlying_2022_2026.csv"
    with out.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=EQUITY_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(levels)
    print(f"  wrote {len(levels):,} sessions to {out.name}")


if __name__ == "__main__":
    main()
