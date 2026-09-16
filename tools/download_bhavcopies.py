"""Download BANKNIFTY CE option chain via daily F&O bhavcopies."""

import argparse
import io
import os
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from datetime import date, timedelta
from pathlib import Path

import pandas as pd
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
from jugaad_data.nse import bhavcopy_fo_raw

# ---------------------------------------------------------
# Default timeout for any requests call that doesn't set one
# (covers jugaad_data internals). (connect, read) seconds.
# ---------------------------------------------------------
DEFAULT_SOCKET_TIMEOUT = (5.0, 20.0)
_original_send = requests.Session.send


def _send_with_default_timeout(self, request, **kwargs):
    if kwargs.get("timeout") is None:
        kwargs["timeout"] = DEFAULT_SOCKET_TIMEOUT
    return _original_send(self, request, **kwargs)


requests.Session.send = _send_with_default_timeout


SYMBOL = "BANKNIFTY"
OPTION_TYPE = "CE"

INDICES = {"BANKNIFTY", "NIFTY", "FINNIFTY", "MIDCPNIFTY", "NIFTYNXT50"}
IS_INDEX = SYMBOL in INDICES

LEGACY_INSTRUMENT = "OPTIDX" if IS_INDEX else "OPTSTK"
# UDiFF FinInstrmTp codes: IDO=index option, STO=stock option, IDF/STF=futures
UDIFF_INSTRUMENT = "IDO" if IS_INDEX else "STO"

START_DATE = date(2022, 1, 1)
END_DATE = date.today()
MAX_WORKERS = 4

FETCH_ATTEMPTS = 2        # per day, on top of urllib3's own retries
STALL_TIMEOUT = 120.0     # pool-wide: abandon everything if nothing completes this long
POLL_INTERVAL = 5.0
HEARTBEAT_EVERY = 15.0    # print a status line if nothing completed for this long
PROGRESS_EVERY = 25

OUT_DIR = Path("../data/sample")

UDIFF_CUTOVER = date(2024, 7, 8)
UDIFF_HOSTS = [
    "https://nsearchives.nseindia.com",
    "https://archives.nseindia.com",
]
UDIFF_PATH_TMPL = "/content/fo/BhavCopy_NSE_FO_0_0_0_{ymd}_F_0000.csv.zip"
UDIFF_TIMEOUT = (5.0, 20.0)
_udiff_host: str | None = None  # chosen by _probe_udiff() before the pool starts

session = requests.Session()
retries = Retry(total=2, connect=1, read=1, backoff_factor=0.5,
                status_forcelist=[429, 500, 502, 503, 504])
session.mount("https://", HTTPAdapter(max_retries=retries,
                                      pool_connections=MAX_WORKERS,
                                      pool_maxsize=MAX_WORKERS))
session.headers.update({
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/124.0.0.0 Safari/537.36"),
    "Referer": "https://www.nseindia.com/all-reports-derivatives",
    "Accept": "*/*",
})

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
    "TckrSymb": "SYMBOL",  # UDiFF has no UndrlygSymb; UndrlygPric is the spot price
}


def _normalize_legacy(df: pd.DataFrame) -> pd.DataFrame:
    df.columns = df.columns.str.strip()
    df = df[df["INSTRUMENT"].astype(str).str.strip() == LEGACY_INSTRUMENT]
    df = df[df["SYMBOL"].astype(str).str.strip() == SYMBOL]
    df = df[df["OPTION_TYP"].astype(str).str.strip() == OPTION_TYPE]
    if df.empty:
        return df
    df = df.rename(columns=LEGACY_COL_MAP).copy()
    df["LTP"] = df["CLOSE"]
    df["MARKET LOT"] = 0
    df["DATE"] = pd.to_datetime(df["DATE"], format="%d-%b-%Y").dt.strftime("%Y-%m-%d")
    df["EXPIRY"] = pd.to_datetime(df["EXPIRY"], format="%d-%b-%Y").dt.strftime("%Y-%m-%d")
    return df[REQUIRED_OPTION_COLUMNS]


class SchemaError(Exception):
    pass


def _normalize_udiff(df: pd.DataFrame) -> pd.DataFrame:
    df.columns = df.columns.str.strip()
    needed = set(UDIFF_COL_MAP) | {"FinInstrmTp"}
    missing = sorted(needed - set(df.columns))
    if missing:
        raise SchemaError(f"UDiFF columns missing {missing}; file has {list(df.columns)}")
    df = df[df["FinInstrmTp"].astype(str).str.strip() == UDIFF_INSTRUMENT]
    df = df[df["TckrSymb"].astype(str).str.strip() == SYMBOL]
    df = df[df["OptnTp"].astype(str).str.strip() == OPTION_TYPE]
    if df.empty:
        return df
    df = df.rename(columns=UDIFF_COL_MAP).copy()
    df["DATE"] = pd.to_datetime(df["DATE"]).dt.strftime("%Y-%m-%d")
    df["EXPIRY"] = pd.to_datetime(df["EXPIRY"]).dt.strftime("%Y-%m-%d")
    return df[REQUIRED_OPTION_COLUMNS]


def _download_udiff_raw(day: date, host: str | None = None) -> pd.DataFrame | None:
    host = host or _udiff_host
    url = host + UDIFF_PATH_TMPL.format(ymd=day.strftime("%Y%m%d"))
    r = session.get(url, timeout=UDIFF_TIMEOUT)
    if r.status_code == 404:
        return None
    r.raise_for_status()
    if len(r.content) < 1024:
        # NSE sometimes returns a tiny HTML/error page with 200
        return None
    with zipfile.ZipFile(io.BytesIO(r.content)) as z:
        name = next(n for n in z.namelist() if n.lower().endswith(".csv"))
        with z.open(name) as f:
            return pd.read_csv(f, low_memory=False)


def _fetch_udiff(day: date, host: str | None = None) -> pd.DataFrame | None:
    raw = _download_udiff_raw(day, host)
    return None if raw is None else _normalize_udiff(raw)


def _fetch_legacy(day: date) -> pd.DataFrame | None:
    text = bhavcopy_fo_raw(day)
    if not text:
        return None
    df = pd.read_csv(io.StringIO(text), low_memory=False)
    return _normalize_legacy(df)


class FetchError(Exception):
    pass


def fetch_day(day: date) -> pd.DataFrame | None:
    last_exc: Exception | None = None
    for attempt in range(FETCH_ATTEMPTS):
        try:
            if day < UDIFF_CUTOVER:
                time.sleep(0.05)
                return _fetch_legacy(day)
            time.sleep(0.1)
            return _fetch_udiff(day)
        except zipfile.BadZipFile:
            return None
        except SchemaError:
            raise  # retrying won't fix a parsing bug
        except Exception as exc:
            last_exc = exc
            time.sleep(1.0 * (attempt + 1))
    raise FetchError(f"{day}: {type(last_exc).__name__}: {last_exc}")


def _probe_udiff(todo: list[date]) -> bool:
    """Pick a working UDiFF host before handing hundreds of days to the pool.

    Returns False if no host serves the post-cutover files, so the caller can
    skip them instead of burning hours on slow failures.
    """
    global _udiff_host
    candidates = [d for d in todo if d >= UDIFF_CUTOVER][:5]  # a few, in case the first is a holiday
    if not candidates:
        return True

    for host in UDIFF_HOSTS:
        for d in candidates:
            t0 = time.monotonic()
            try:
                raw = _download_udiff_raw(d, host=host)
            except (requests.RequestException, zipfile.BadZipFile, StopIteration) as exc:
                print(f"  probe {host} {d}: FAILED in {time.monotonic() - t0:.1f}s "
                      f"-> {type(exc).__name__}: {exc}")
                break  # network/host problem; try the next host
            dt = time.monotonic() - t0
            if raw is None:
                print(f"  probe {host} {d}: no file ({dt:.1f}s), trying next day")
                continue
            # Parsing errors are code bugs, not host problems: let them raise loudly.
            df = _normalize_udiff(raw)
            print(f"  probe {host} {d}: OK, {len(df)} rows in {dt:.1f}s -> using this host")
            if df.empty:
                print(f"  !! filter matched 0 rows. FinInstrmTp values in file: "
                      f"{sorted(raw['FinInstrmTp'].astype(str).str.strip().unique())}; "
                      f"{SYMBOL} present in TckrSymb: "
                      f"{(raw['TckrSymb'].astype(str).str.strip() == SYMBOL).any()}")
            _udiff_host = host
            return True
    return False


def _out_file() -> Path:
    return OUT_DIR / f"{SYMBOL.lower()}_{OPTION_TYPE.lower()}_{START_DATE.year}_{END_DATE.year}.csv"


def _load_existing(out_file: Path) -> pd.DataFrame | None:
    if not out_file.exists():
        return None
    try:
        existing = pd.read_csv(out_file, low_memory=False)
    except Exception as exc:
        print(f"  could not read {out_file.name} ({exc}); downloading the full range")
        return None
    missing = [c for c in REQUIRED_OPTION_COLUMNS if c not in existing.columns]
    if missing:
        print(f"  {out_file.name} is missing columns {missing}; downloading the full range")
        return None
    return existing[REQUIRED_OPTION_COLUMNS]


def _run_pool(todo: list[date]):
    """Returns (frames, empty_days, closed_days, errors, hung_days)."""
    pool = ThreadPoolExecutor(max_workers=MAX_WORKERS)
    futures = {pool.submit(fetch_day, d): d for d in todo}
    pending = set(futures)

    frames: list[pd.DataFrame] = []
    rows = 0
    empty_days = closed_days = 0
    errors: list[str] = []
    hung: list[date] = []
    done = 0
    started = time.monotonic()
    last_progress = started
    last_heartbeat = started

    def status() -> str:
        elapsed = time.monotonic() - started
        return (f"[{done}/{len(todo)}] rows={rows} empty={empty_days} "
                f"closed={closed_days} errors={len(errors)} elapsed={elapsed:.0f}s")

    while pending:
        finished, pending = wait(pending, timeout=POLL_INTERVAL, return_when=FIRST_COMPLETED)
        now = time.monotonic()

        if not finished:
            idle = now - last_progress
            if idle > STALL_TIMEOUT:
                print(f"\n!! No completions for {idle:.0f}s; abandoning "
                      f"{len(pending)} outstanding day(s).")
                for fut in pending:
                    day = futures[fut]
                    hung.append(day)
                    errors.append(f"{day}: abandoned after pool-wide stall")
                pending = set()
            elif now - last_heartbeat > HEARTBEAT_EVERY:
                print(f"  ... waiting ({idle:.0f}s since last completion) {status()}")
                last_heartbeat = now
            continue

        last_progress = last_heartbeat = now
        for fut in finished:
            done += 1
            day = futures[fut]
            try:
                df = fut.result()
            except FetchError as exc:
                errors.append(str(exc))
                print(f"  ERR {exc}")
                continue
            except Exception as exc:
                msg = f"{day}: unexpected {type(exc).__name__}: {exc}"
                errors.append(msg)
                print(f"  ERR {msg}")
                continue

            if df is None:
                closed_days += 1
            elif df.empty:
                empty_days += 1
            else:
                frames.append(df)
                rows += len(df)

            if done % PROGRESS_EVERY == 0 or done == len(todo):
                print(f"  {status()}")

    pool.shutdown(wait=False, cancel_futures=True)
    return frames, empty_days, closed_days, errors, hung


def main(rebuild: bool = False) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_file = _out_file()

    all_days = [START_DATE + timedelta(days=i) for i in range((END_DATE - START_DATE).days + 1)]
    trading_days = [d for d in all_days if d.weekday() < 5]

    existing = None if rebuild else _load_existing(out_file)
    have: set[str] = set()
    if existing is not None:
        have = set(existing["DATE"].astype(str))
        print(f"Found {len(existing)} rows covering {len(have)} days in {out_file.name}")

    todo = [d for d in trading_days if d.strftime("%Y-%m-%d") not in have]
    skipped = len(trading_days) - len(todo)

    if not todo:
        print(f"Nothing to fetch: all {len(trading_days)} weekdays in "
              f"{START_DATE}..{END_DATE} are already on file.")
        return

    udiff_skipped = 0
    if any(d >= UDIFF_CUTOVER for d in todo):
        print("Probing UDiFF hosts...")
        if not _probe_udiff(todo):
            before = len(todo)
            todo = [d for d in todo if d < UDIFF_CUTOVER]
            udiff_skipped = before - len(todo)
            print(f"!! No UDiFF host worked; skipping {udiff_skipped} day(s) from "
                  f"{UDIFF_CUTOVER} onward. Legacy days will still be fetched and saved.")

    if not todo:
        print("Nothing left to fetch after skipping UDiFF days.")
        return

    print(f"Fetching {SYMBOL} {OPTION_TYPE} rows for {len(todo)} day(s) "
          f"({skipped} already on file) using {MAX_WORKERS} workers...")

    frames, empty_days, closed_days, errors, hung = _run_pool(todo)

    if not frames and existing is None:
        raise RuntimeError("No option data collected across the entire range")

    parts = [existing] if existing is not None else []
    parts += frames
    combined = pd.concat(parts, ignore_index=True)

    before = len(combined)
    combined = combined.drop_duplicates(
        subset=["DATE", "EXPIRY", "OPTION TYPE", "STRIKE PRICE"], keep="last")
    duplicates = before - len(combined)

    combined = combined.sort_values(["DATE", "EXPIRY", "STRIKE PRICE"]).reset_index(drop=True)
    added = len(combined) - (0 if existing is None else len(existing))

    tmp_file = out_file.with_suffix(".csv.tmp")
    combined.to_csv(tmp_file, index=False, lineterminator="\n")
    try:
        os.replace(tmp_file, out_file)
    except OSError as exc:
        print(f"\nCould not replace {out_file.name}: {exc}")
        print(f"The merged data is safe at: {tmp_file.resolve()}")
        print("Close the file in Excel/your editor, then rename the .tmp over it.")
        return

    print(f"\nWrote {len(combined)} rows ({added:+d}) across "
          f"{combined['EXPIRY'].nunique()} expiries x "
          f"{combined['STRIKE PRICE'].nunique()} unique strikes to {out_file.resolve()}")
    print(f"Summary: {skipped} days already on file, {len(frames)} days newly fetched, "
          f"{empty_days} days with no {SYMBOL} {OPTION_TYPE}, "
          f"{closed_days} days closed/no file")
    if udiff_skipped:
        print(f"  {udiff_skipped} UDiFF day(s) skipped (no working host); rerun to retry")
    if duplicates:
        print(f"  collapsed {duplicates} duplicate contract-day rows")
    if hung:
        print(f"\n!! {len(hung)} day(s) abandoned after a pool-wide stall (rerun to retry):")
        for d in sorted(hung):
            print(f"     {d}")
    if errors:
        print(f"\n!! {len(errors)} day(s) could not be fetched (rerun to retry):")
        for e in sorted(errors):
            print(f"     {e}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rebuild", action="store_true",
                        help="ignore the existing file and re-download the whole range")
    main(rebuild=parser.parse_args().rebuild)
    sys.stdout.flush()
    # Skip interpreter shutdown, which would join any wedged worker threads.
    # Safe: the CSV is already written atomically via os.replace.
    os._exit(0)