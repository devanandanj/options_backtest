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
# Monkey-patch default timeout into requests so third-party
# libraries (jugaad_data) never block indefinitely.
# NOTE: this does NOT cover DNS resolution (socket.getaddrinfo
# runs before the timeout-bound socket exists), which is the
# most likely cause of a truly unkillable hang.
# ---------------------------------------------------------
DEFAULT_SOCKET_TIMEOUT = 8.0
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
UDIFF_INSTRUMENT = "IDXO" if IS_INDEX else "STO"

START_DATE = date(2022, 1, 1)
END_DATE = date.today()
MAX_WORKERS = 4  # Reduced slightly to prevent rate-limit throttling

# If NO future completes anywhere in the pool for this long, we assume every
# worker thread is wedged (e.g. a DNS hang) and give up on everything still
# outstanding rather than waiting forever. This is a POOL-WIDE stall detector,
# not a per-task timeout — a per-future timeout can't help once all worker
# threads are simultaneously stuck, since no new task can ever get a thread.
STALL_TIMEOUT = 60.0
POLL_INTERVAL = 5.0

OUT_DIR = Path("../data/sample")

UDIFF_CUTOVER = date(2024, 7, 8)
UDIFF_URL_TMPL = (
    "https://archives.nseindia.com/content/fo/"
    "BhavCopy_NSE_FO_0_0_0_{ymd}_F_0000.csv.zip"
)

# Persistent session with retries for UDiff
session = requests.Session()
retries = Retry(total=3, backoff_factor=0.5, status_forcelist=[429, 500, 502, 503, 504])
session.mount("https://", HTTPAdapter(max_retries=retries, pool_connections=MAX_WORKERS, pool_maxsize=MAX_WORKERS))
session.headers.update({
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/124.0.0.0 Safari/537.36"),
    "Referer": "https://www.nseindia.com/all-reports",
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
    "UndrlygSymb": "SYMBOL",
}


def _all_expiries(df: pd.DataFrame) -> pd.DataFrame:
    return df


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
    return _all_expiries(df[REQUIRED_OPTION_COLUMNS])


def _normalize_udiff(df: pd.DataFrame) -> pd.DataFrame:
    df.columns = df.columns.str.strip()
    df = df[df["FinInstrmTp"].astype(str).str.strip() == UDIFF_INSTRUMENT]
    df = df[df["UndrlygSymb"].astype(str).str.strip() == SYMBOL]
    df = df[df["OptnTp"].astype(str).str.strip() == OPTION_TYPE]
    if df.empty:
        return df
    df = df.rename(columns=UDIFF_COL_MAP).copy()
    df["DATE"] = pd.to_datetime(df["DATE"]).dt.strftime("%Y-%m-%d")
    df["EXPIRY"] = pd.to_datetime(df["EXPIRY"]).dt.strftime("%Y-%m-%d")
    return _all_expiries(df[REQUIRED_OPTION_COLUMNS])


def _fetch_udiff(day: date) -> pd.DataFrame | None:
    url = UDIFF_URL_TMPL.format(ymd=day.strftime("%Y%m%d"))
    r = session.get(url, timeout=12)
    if r.status_code == 404 or len(r.content) < 1024:
        return None
    r.raise_for_status()
    with zipfile.ZipFile(io.BytesIO(r.content)) as z:
        name = next(n for n in z.namelist() if n.endswith(".csv"))
        with z.open(name) as f:
            df = pd.read_csv(f, low_memory=False)
    return _normalize_udiff(df)


def _fetch_legacy(day: date) -> pd.DataFrame | None:
    text = bhavcopy_fo_raw(day)
    if not text:
        return None
    df = pd.read_csv(io.StringIO(text), low_memory=False)
    return _normalize_legacy(df)


class FetchError(Exception):
    pass


def fetch_day(day: date) -> pd.DataFrame | None:
    if day.weekday() >= 5:
        return None

    last_exc: Exception | None = None
    for attempt in range(3):
        try:
            if day < UDIFF_CUTOVER:
                time.sleep(0.05)
                return _fetch_legacy(day)
            time.sleep(0.1)
            return _fetch_udiff(day)
        except zipfile.BadZipFile:
            return None
        except Exception as exc:
            last_exc = exc
            time.sleep(1.0 * (attempt + 1))
    raise FetchError(f"{day}: {type(last_exc).__name__}: {last_exc}")


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
    """Run fetch_day over todo with a pool-wide stall watchdog.

    Returns (frames, empty_days, closed_days, errors, hung_days).
    Deliberately does NOT use `with ThreadPoolExecutor(...)`: that calls
    shutdown(wait=True) on exit, and concurrent.futures.thread also joins
    every worker thread it ever created at interpreter atexit — so a truly
    wedged thread (e.g. a hung DNS lookup) will still hang the script at
    cleanup even if we stop waiting on it here. We call os._exit() at the
    very end of the script to bypass that entirely.
    """
    pool = ThreadPoolExecutor(max_workers=MAX_WORKERS)
    futures = {pool.submit(fetch_day, d): d for d in todo}
    pending = set(futures)

    frames: list[pd.DataFrame] = []
    empty_days = closed_days = 0
    errors: list[str] = []
    hung: list[date] = []
    done = 0
    last_progress = time.monotonic()

    while pending:
        finished, pending = wait(pending, timeout=POLL_INTERVAL, return_when=FIRST_COMPLETED)

        if not finished:
            stalled_for = time.monotonic() - last_progress
            if stalled_for > STALL_TIMEOUT:
                print(f"\n!! No progress for {stalled_for:.0f}s — assuming the remaining "
                      f"{len(pending)} worker(s) are wedged (likely a DNS/connect hang "
                      f"not covered by the requests timeout). Abandoning them.")
                for fut in pending:
                    day = futures[fut]
                    hung.append(day)
                    errors.append(f"{day}: abandoned after pool-wide stall")
                pending = set()
            continue

        last_progress = time.monotonic()
        for fut in finished:
            done += 1
            day = futures[fut]
            try:
                df = fut.result()
            except FetchError as exc:
                errors.append(str(exc))
                continue
            except Exception as exc:
                errors.append(f"{day}: unexpected {type(exc).__name__}: {exc}")
                continue

            if df is None:
                closed_days += 1
            elif df.empty:
                empty_days += 1
            else:
                frames.append(df)

            if done % 50 == 0 or done == len(todo):
                print(f"  [{done}/{len(todo)}] rows-collected={sum(len(f) for f in frames)} "
                      f"empty-days={empty_days} closed-days={closed_days} errors={len(errors)}")

    # Don't block on stuck threads: shut down without joining. Any wedged
    # threads become orphans; os._exit() at the bottom of the script kills
    # them along with the process instead of waiting for them to finish.
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

    print(f"Fetching {SYMBOL} {OPTION_TYPE} rows for {len(todo)} day(s) not yet on file "
          f"({skipped} skipped) using {MAX_WORKERS} workers...")

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

    combined = combined.sort_values(
        ["DATE", "EXPIRY", "STRIKE PRICE"], ascending=[True, True, True]
    ).reset_index(drop=True)

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
          f"{closed_days} days the exchange was closed")
    if duplicates:
        print(f"  collapsed {duplicates} duplicate contract-day rows")
    if hung:
        print(f"\n!! {len(hung)} day(s) were abandoned due to a pool-wide stall "
              f"(rerun the script — they'll be retried, and already-fetched days are skipped):")
        for d in sorted(hung):
            print(f"     {d}")
    if errors:
        print(f"\n!! {len(errors)} day(s) could not be fetched:")
        for e in sorted(errors):
            print(f"     {e}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rebuild", action="store_true",
                        help="ignore the existing file and re-download the whole range")
    main(rebuild=parser.parse_args().rebuild)
    sys.stdout.flush()
    # Force-terminate. If any fetch_day threads are wedged (e.g. a DNS hang
    # around the UDiFF cutover), a normal process exit would block forever
    # joining them (concurrent.futures.thread registers an atexit handler
    # that waits on every thread any pool ever spawned). os._exit() skips
    # atexit/cleanup entirely and just kills the process — safe here since
    # the CSV write above already completed via os.replace (atomic).
    os._exit(0)