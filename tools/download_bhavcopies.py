"""Download IDFCFIRSTB CE option chain via daily F&O bhavcopies.

Faster and more complete than the per-contract path in download_data.py: each
day's bhavcopy carries every strike and every expiry in a single response, so
one download per trading day covers the whole chain — no strike-range guessing
and no per-month expiry-day probing.

Two format eras:
- Legacy `fo{DDMONYYYY}.csv.zip` (2022 through Jul 2024): fetched via
  `jugaad_data.nse.bhavcopy_fo_raw()` which handles the URL and caches locally.
- UDiff `BhavCopy_NSE_FO_0_0_0_{YYYYMMDD}_F_0000.csv.zip` (Jul 2024+): fetched
  by direct HTTPS request; jugaad_data doesn't know this format yet.

Output: one 16-column chain CSV in the format load_chain_csv (src/data/loader.cpp)
expects, filtered to IDFCFIRSTB CE and to front-month rows (trade-month =
expiry-month) so strike_strategy's per-month premium lookup isn't ambiguous
across overlapping expiries.
"""

import io
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import date, timedelta
from pathlib import Path

import pandas as pd
import requests
from jugaad_data.nse import bhavcopy_fo_raw

SYMBOL = "IDFCFIRSTB"
OPTION_TYPE = "CE"

START_DATE = date(2022, 1, 1)
END_DATE = date.today()
MAX_WORKERS = 6

OUT_DIR = Path("../data/sample")

# NSE switched from the legacy bhavcopy to the UDiff format around Jul 2024.
# Before this date use jugaad_data (cached); after, hit the UDiff URL directly.
UDIFF_CUTOVER = date(2024, 7, 8)

UDIFF_URL_TMPL = (
    "https://archives.nseindia.com/content/fo/"
    "BhavCopy_NSE_FO_0_0_0_{ymd}_F_0000.csv.zip"
)

UDIFF_HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/124.0.0.0 Safari/537.36"),
    "Referer": "https://www.nseindia.com/all-reports",
}

REQUIRED_OPTION_COLUMNS = [
    "DATE", "EXPIRY", "OPTION TYPE", "STRIKE PRICE",
    "OPEN", "HIGH", "LOW", "CLOSE", "LTP", "SETTLE PRICE",
    "TOTAL TRADED QUANTITY", "MARKET LOT", "PREMIUM VALUE",
    "OPEN INTEREST", "CHANGE IN OI", "SYMBOL",
]

LEGACY_COL_MAP = {
    "TIMESTAMP": "DATE",
    "EXPIRY_DT": "EXPIRY",
    "OPTION_TYP": "OPTION TYPE",
    "STRIKE_PR": "STRIKE PRICE",
    "SETTLE_PR": "SETTLE PRICE",
    "CONTRACTS": "TOTAL TRADED QUANTITY",
    "VAL_INLAKH": "PREMIUM VALUE",
    "OPEN_INT": "OPEN INTEREST",
    "CHG_IN_OI": "CHANGE IN OI",
    "SYMBOL": "SYMBOL",
}

UDIFF_COL_MAP = {
    "TradDt": "DATE",
    "XpryDt": "EXPIRY",
    "OptnTp": "OPTION TYPE",
    "StrkPric": "STRIKE PRICE",
    "OpnPric": "OPEN",
    "HghPric": "HIGH",
    "LwPric": "LOW",
    "ClsPric": "CLOSE",
    "LastPric": "LTP",
    "SttlmPric": "SETTLE PRICE",
    "TtlTradgVol": "TOTAL TRADED QUANTITY",
    "NewBrdLotQty": "MARKET LOT",
    "TtlTrfVal": "PREMIUM VALUE",
    "OpnIntrst": "OPEN INTEREST",
    "ChngInOpnIntrst": "CHANGE IN OI",
    "TckrSymb": "SYMBOL",
}


def _front_month_only(df: pd.DataFrame) -> pd.DataFrame:
    """Keep only rows where the contract expires in the same calendar month as
    the trade date. Mirrors download_data.py's per-contract semantics so the
    strategy's premium_at_month_start lookup can't pick a far-month contract."""
    trade = pd.to_datetime(df["DATE"])
    expiry = pd.to_datetime(df["EXPIRY"])
    same_month = (trade.dt.year == expiry.dt.year) & (trade.dt.month == expiry.dt.month)
    return df[same_month]


def _normalize_legacy(df: pd.DataFrame) -> pd.DataFrame:
    df = df[df["INSTRUMENT"] == "OPTSTK"]
    df = df[df["SYMBOL"] == SYMBOL]
    df = df[df["OPTION_TYP"] == OPTION_TYPE]
    if df.empty:
        return df
    df = df.rename(columns=LEGACY_COL_MAP).copy()
    # Legacy bhavcopy has no LTP or MARKET LOT columns; use CLOSE as LTP proxy
    # and leave MARKET LOT at 0 — strike_strategy doesn't consume either.
    df["LTP"] = df["CLOSE"]
    df["MARKET LOT"] = 0
    df["DATE"] = pd.to_datetime(df["DATE"], format="%d-%b-%Y").dt.strftime("%Y-%m-%d")
    df["EXPIRY"] = pd.to_datetime(df["EXPIRY"], format="%d-%b-%Y").dt.strftime("%Y-%m-%d")
    return _front_month_only(df[REQUIRED_OPTION_COLUMNS])


def _normalize_udiff(df: pd.DataFrame) -> pd.DataFrame:
    df = df[df["FinInstrmTp"] == "STO"]  # STO = stock option
    df = df[df["TckrSymb"] == SYMBOL]
    df = df[df["OptnTp"] == OPTION_TYPE]
    if df.empty:
        return df
    df = df.rename(columns=UDIFF_COL_MAP).copy()
    df["DATE"] = pd.to_datetime(df["DATE"]).dt.strftime("%Y-%m-%d")
    df["EXPIRY"] = pd.to_datetime(df["EXPIRY"]).dt.strftime("%Y-%m-%d")
    return _front_month_only(df[REQUIRED_OPTION_COLUMNS])


def _fetch_udiff(day: date) -> pd.DataFrame | None:
    url = UDIFF_URL_TMPL.format(ymd=day.strftime("%Y%m%d"))
    r = requests.get(url, headers=UDIFF_HEADERS, timeout=20)
    if r.status_code == 404 or len(r.content) < 1024:
        return None  # exchange holiday or missing bhavcopy
    r.raise_for_status()
    with zipfile.ZipFile(io.BytesIO(r.content)) as z:
        name = next(n for n in z.namelist() if n.endswith(".csv"))
        with z.open(name) as f:
            df = pd.read_csv(f, low_memory=False)
    return _normalize_udiff(df)


def _fetch_legacy(day: date) -> pd.DataFrame | None:
    text = bhavcopy_fo_raw(day)  # jugaad_data caches locally
    if not text:
        return None
    df = pd.read_csv(io.StringIO(text), low_memory=False)
    return _normalize_legacy(df)


def fetch_day(day: date) -> pd.DataFrame | None:
    if day.weekday() >= 5:  # Sat/Sun
        return None
    try:
        if day < UDIFF_CUTOVER:
            return _fetch_legacy(day)
        # Small pace between UDiff fetches inside a worker thread — the pool has
        # 6 workers, so the effective request cadence stays under NSE's WAF.
        time.sleep(0.1)
        return _fetch_udiff(day)
    except Exception:
        return None  # exchange holiday, transient error, or format edge case


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    all_days = [START_DATE + timedelta(days=i) for i in range((END_DATE - START_DATE).days + 1)]
    trading_days = [d for d in all_days if d.weekday() < 5]

    print(f"Fetching {SYMBOL} {OPTION_TYPE} rows across {len(trading_days)} trading days "
          f"({START_DATE}..{END_DATE}) using {MAX_WORKERS} workers...")

    frames: list[pd.DataFrame] = []
    empty_days = failed_days = 0
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as pool:
        futures = {pool.submit(fetch_day, d): d for d in trading_days}
        done = 0
        for fut in as_completed(futures):
            done += 1
            df = fut.result()
            if df is None:
                failed_days += 1
            elif df.empty:
                empty_days += 1
            else:
                frames.append(df)
            if done % 100 == 0:
                print(f"  [{done}/{len(trading_days)}] rows-collected={sum(len(f) for f in frames)} "
                      f"empty-days={empty_days} failed-days={failed_days}")

    if not frames:
        raise RuntimeError("No option data collected across the entire range")

    combined = pd.concat(frames, ignore_index=True)
    combined = combined.sort_values(
        ["DATE", "EXPIRY", "STRIKE PRICE"], ascending=[True, True, True]
    ).reset_index(drop=True)

    out_file = OUT_DIR / f"{SYMBOL.lower()}_{OPTION_TYPE.lower()}_{START_DATE.year}_{END_DATE.year}.csv"
    combined.to_csv(out_file, index=False, lineterminator="\n")
    print(f"\nWrote {len(combined)} rows across {combined['EXPIRY'].nunique()} expiries × "
          f"{combined['STRIKE PRICE'].nunique()} unique strikes to {out_file.resolve()}")
    print(f"Summary: {done - failed_days - empty_days} days with data, "
          f"{empty_days} days with no IDFCFIRSTB CE, {failed_days} failed/holidays")


if __name__ == "__main__":
    main()
