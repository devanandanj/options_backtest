import calendar
import math
import time
from datetime import date, timedelta
from pathlib import Path

import pandas as pd
from jugaad_data.nse import stock_df, derivatives_df

SYMBOL = "BANKNIFTY"

EQUITY_FROM = date(2022, 1, 1)
EQUITY_TO = date.today()

OPTION_TYPE = "CE"                 # "CE" for calls, "PE" for puts
STRIKE_STEP = 5.0                  # IDFCFIRSTB stock-option spacing
STRIKE_RANGE_PCT = 0.25            # ±25% around spot; wide enough that the strategy's
                                   # +10% OTM target, ceilinged to the next 5-step and drifted
                                   # further OTM by month-end moves, still falls inside the fetched
                                   # chain instead of running off the top of the download range.

OUT_DIR = Path("../data/sample")

REQUIRED_OPTION_COLUMNS = [
    "DATE", "EXPIRY", "OPTION TYPE", "STRIKE PRICE",
    "OPEN", "HIGH", "LOW", "CLOSE", "LTP", "SETTLE PRICE",
    "TOTAL TRADED QUANTITY", "MARKET LOT", "PREMIUM VALUE",
    "OPEN INTEREST", "CHANGE IN OI", "SYMBOL",
]


def _month_spans(start: date, end: date) -> list[tuple[date, date]]:
    """The range broken into calendar months, inclusive at both ends."""
    spans: list[tuple[date, date]] = []
    cursor = start.replace(day=1)
    while cursor <= end:
        last = cursor.replace(day=calendar.monthrange(cursor.year, cursor.month)[1])
        spans.append((max(cursor, start), min(last, end)))
        cursor = last + timedelta(days=1)
    return spans


def _ist_dates(df: pd.DataFrame) -> pd.Series:
    """DATE as "YYYY-MM-DD" in IST.

    stock_df hands back IST midnight rendered in UTC, i.e. "2022-01-02 18:30:00"
    for Monday 2022-01-03. Truncating that naively lands on the previous calendar
    day, which silently shifts every bar back one trading day and fills the file
    with Sundays.
    """
    return (pd.to_datetime(df["DATE"], utc=True)
              .dt.tz_convert("Asia/Kolkata")
              .dt.strftime("%Y-%m-%d"))


def _fetch_month(span_from: date, span_to: date) -> pd.DataFrame:
    """Rows for one calendar month, verified to actually be from that month.

    NSE's history endpoint sometimes answers a window with a different month's
    data: 2026-05-01..2026-05-31 returns April, reproducibly, while shifting
    either endpoint by a day returns May. Both of those endpoints are non-trading
    days (Maharashtra Day and a Sunday), which appears to be the trigger.

    Rather than trust any single window, ask, check what came back, and fall back
    to nudged endpoints until the response is really from the month requested.
    Windows stay inside one calendar month so stock_df's own chunker leaves them
    alone; a wider window would just be split back into the failing pair.
    """
    month = span_from.strftime("%Y-%m")
    attempts = [
        (span_from, span_to),
        (span_from, span_to - timedelta(days=1)),
        (span_from + timedelta(days=1), span_to),
    ]
    collected: list[pd.DataFrame] = []
    for attempt_from, attempt_to in attempts:
        if attempt_from > attempt_to:
            continue
        chunk = stock_df(symbol=SYMBOL, from_date=attempt_from,
                         to_date=attempt_to, series="EQ")
        if "SERIES" in chunk.columns:
            chunk = chunk[chunk["SERIES"] == "EQ"]
        if chunk.empty:
            continue
        chunk = chunk.assign(DATE=_ist_dates(chunk))
        chunk = chunk[chunk["DATE"].str[:7] == month]
        if chunk.empty:
            continue  # the endpoint answered with some other month
        collected.append(chunk)
        # A nudged window drops the day it trimmed, so keep going to recover it.
        if (attempt_from, attempt_to) == (span_from, span_to):
            break
    if not collected:
        return pd.DataFrame()
    return pd.concat(collected, ignore_index=True)


def fetch_equity() -> pd.DataFrame:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Fetching {SYMBOL} equity from {EQUITY_FROM} to {EQUITY_TO}...")

    # One request per calendar month rather than one for the whole range. stock_df
    # chunks a long range itself and gets it wrong at some boundaries: a single
    # 2022-01-01..2026-09-12 request returned April 2026 twice and omitted May
    # entirely. Per-month fetches are verifiable, and a gap cannot pass unnoticed.
    frames: list[pd.DataFrame] = []
    empty_months: list[str] = []
    for span_from, span_to in _month_spans(EQUITY_FROM, EQUITY_TO):
        chunk = _fetch_month(span_from, span_to)
        if chunk.empty:
            empty_months.append(span_from.strftime("%Y-%m"))
        else:
            frames.append(chunk)

    if not frames:
        raise RuntimeError("No equity records returned")
    if empty_months:
        print(f"  months with no rows: {', '.join(empty_months)}")

    df = pd.concat(frames, ignore_index=True)

    # Fallback windows overlap by design, so the same day can arrive twice; the
    # strategy reads one close per day, so collapse them rather than let a doubled
    # month distort anything downstream.
    before = len(df)
    df = df.drop_duplicates(subset="DATE", keep="first")
    if before != len(df):
        print(f"  dropped {before - len(df)} duplicate date rows")
    df = df.sort_values("DATE").reset_index(drop=True)

    present = set(df["DATE"].str[:7])
    missing = [f.strftime("%Y-%m") for f, _ in _month_spans(EQUITY_FROM, EQUITY_TO)
               if f.strftime("%Y-%m") not in present]
    if missing:
        raise RuntimeError(
            f"No equity rows for {', '.join(missing)} - refusing to write a file with "
            f"calendar gaps. month_offset counts months present in the data, so a gap "
            f"silently lengthens the holding period: with May absent, an April entry at "
            f"offset 1 is checked against June.")

    out_file = OUT_DIR / f"{SYMBOL.lower()}_underlying_{EQUITY_FROM.year}_{EQUITY_TO.year}.csv"
    df.to_csv(out_file, index=False, lineterminator="\n")
    print(f"Wrote {len(df)} equity rows to {out_file.resolve()}\n")
    return df


def _last_thursday(year: int, month: int) -> date:
    last_day = calendar.monthrange(year, month)[1]
    d = date(year, month, last_day)
    while d.weekday() != 3:  # Monday=0, Thursday=3
        d -= timedelta(days=1)
    return d


def _monthly_expiries(start: date, end: date) -> list[date]:
    out: list[date] = []
    y, m = start.year, start.month
    while (y, m) <= (end.year, end.month):
        exp = _last_thursday(y, m)
        if start <= exp <= end:
            out.append(exp)
        m += 1
        if m > 12:
            m, y = 1, y + 1
    return out


def _spot_from_cached_equity(eq_df: pd.DataFrame, expiry: date) -> tuple[date, float] | None:
    """Extracts month-start spot price directly from pre-downloaded equity data."""
    month_start_str = expiry.replace(day=1).strftime("%Y-%m-%d")
    expiry_str = expiry.strftime("%Y-%m-%d")

    # Slice pre-loaded data instead of hitting NSE API
    sub = eq_df[(eq_df["DATE"] >= month_start_str) & (eq_df["DATE"] <= expiry_str)]
    if sub.empty:
        return None

    first = sub.iloc[0]
    return pd.to_datetime(first["DATE"]).date(), float(first["CLOSE"])


def _strikes_around(spot: float, pct: float, step: float) -> list[float]:
    lo = math.floor((spot * (1 - pct)) / step) * step
    hi = math.ceil((spot * (1 + pct)) / step) * step
    n = int(round((hi - lo) / step)) + 1
    return [round(lo + i * step, 2) for i in range(n)]


def _resolve_expiry(guess: date, atm_strike: float) -> tuple[date | None, pd.DataFrame | None]:
    """Find NSE's actual monthly expiry for the month of `guess`.

    NSE stock-option expiry has shifted between last Thursday and last Tuesday
    (SEBI moved stock F&O from Thursday to Tuesday around Sep 2025), and either
    can additionally shift a business day earlier when the target day is an
    exchange holiday. Probe candidates in that order: last Thursday, last
    Tuesday, then walk back a business day from each for the holiday case.
    """
    y, m = guess.year, guess.month
    month_start = date(y, m, 1)

    last_day_of_month = calendar.monthrange(y, m)[1]
    end_of_month = date(y, m, last_day_of_month)

    def last_weekday(target_weekday: int) -> date:
        d = end_of_month
        while d.weekday() != target_weekday:
            d -= timedelta(days=1)
        return d

    THU, TUE = 3, 1
    candidates: list[date] = []
    for anchor in (last_weekday(THU), last_weekday(TUE)):
        for offset in range(3):  # anchor, anchor-1, anchor-2 (holiday shift)
            c = anchor - timedelta(days=offset)
            if month_start <= c <= end_of_month and c not in candidates:
                candidates.append(c)

    for candidate in candidates:
        try:
            df = derivatives_df(
                symbol=SYMBOL,
                from_date=month_start,
                to_date=candidate,
                expiry_date=candidate,
                instrument_type="OPTSTK",
                strike_price=atm_strike,
                option_type=OPTION_TYPE,
            )
            time.sleep(0.2)
        except (KeyError, Exception):
            continue

        if df is not None and not df.empty:
            return candidate, df

    return None, None


def _fetch_expiry_frames(expiry: date, eq_df: pd.DataFrame) -> pd.DataFrame | None:
    ref = _spot_from_cached_equity(eq_df, expiry)
    if ref is None:
        print(f"  [skip expiry {expiry}] no equity data to derive spot")
        return None
    ref_date, spot = ref

    strikes = _strikes_around(spot, STRIKE_RANGE_PCT, STRIKE_STEP)
    atm = min(strikes, key=lambda s: abs(s - spot))

    actual_expiry, atm_df = _resolve_expiry(expiry, atm)
    if actual_expiry is None:
        print(f"  [none] expiry {expiry}: no data at last Thursday or 3 prior days")
        return None
    if actual_expiry != expiry:
        print(f"  [shift] expiry {expiry} -> {actual_expiry} (holiday-adjusted)")

    month_start = actual_expiry.replace(day=1)
    print(f"Expiry {actual_expiry}: spot={spot:.2f} on {ref_date}, checking {len(strikes)} strikes "
          f"({strikes[0]}..{strikes[-1]})")

    frames: list[pd.DataFrame] = []

    # Preserve the probed ATM contract DataFrame to save 1 redundant call
    if atm_df is not None:
        frames.append(atm_df)

    for strike in strikes:
        if strike == atm and atm_df is not None:
            continue  # Already obtained during expiry resolution

        try:
            df = derivatives_df(
                symbol=SYMBOL,
                from_date=month_start,
                to_date=actual_expiry,
                expiry_date=actual_expiry,
                instrument_type="OPTSTK",
                strike_price=strike,
                option_type=OPTION_TYPE,
            )
            time.sleep(0.35)  # Guard against WAF rate-limiting (~625 calls at 25% range)
        except (KeyError, Exception):
            continue

        if df is None or df.empty:
            continue

        frames.append(df)

    if not frames:
        print(f"  [none] expiry {actual_expiry}: no strikes returned data")
        return None

    combined = pd.concat(frames, ignore_index=True)
    print(f"  [ok]   expiry {actual_expiry}: {len(combined)} rows across "
          f"{combined['STRIKE PRICE'].nunique()} strikes")
    return combined


def fetch_option_range(eq_df: pd.DataFrame) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    expiries = _monthly_expiries(EQUITY_FROM, EQUITY_TO)

    est_calls = len(expiries) * len(_strikes_around(100.0, STRIKE_RANGE_PCT, STRIKE_STEP))
    print(f"Downloading {OPTION_TYPE} chain across {len(expiries)} monthly expiries "
          f"({EQUITY_FROM}..{EQUITY_TO})")
    print(f"  ~{est_calls} NSE API calls required.")

    all_frames: list[pd.DataFrame] = []
    for exp in expiries:
        frame = _fetch_expiry_frames(exp, eq_df)
        if frame is not None:
            all_frames.append(frame)

    if not all_frames:
        raise RuntimeError("No option data collected across the entire range")

    combined = pd.concat(all_frames, ignore_index=True)

    # Validate schema integrity
    for col in REQUIRED_OPTION_COLUMNS:
        if col not in combined.columns:
            combined[col] = ""

    combined = combined[REQUIRED_OPTION_COLUMNS].copy()
    combined["DATE"] = pd.to_datetime(combined["DATE"]).dt.strftime("%Y-%m-%d")
    combined["EXPIRY"] = pd.to_datetime(combined["EXPIRY"]).dt.strftime("%Y-%m-%d")

    # Sort chronologically (oldest to newest)
    combined = combined.sort_values(
        ["DATE", "EXPIRY", "STRIKE PRICE"], ascending=[True, True, True]
    ).reset_index(drop=True)

    out_file = OUT_DIR / f"{SYMBOL.lower()}_{OPTION_TYPE.lower()}_{EQUITY_FROM.year}_{EQUITY_TO.year}.csv"
    combined.to_csv(out_file, index=False, lineterminator="\n")
    print(f"\nWrote {len(combined)} rows across {combined['EXPIRY'].nunique()} expiries "
          f"× {combined['STRIKE PRICE'].nunique()} unique strikes to {out_file.resolve()}")


if __name__ == "__main__":
    equity_dataframe = fetch_equity()
    fetch_option_range(equity_dataframe)