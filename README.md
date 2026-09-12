# options-backtest

A C++20 backtesting engine for NSE (India) equity options, with a Python data
pipeline for pulling historical underlying and option-chain data.

The engine currently implements one strategy: systematically writing an
out-of-the-money call each month at a fixed percentage above spot.

---

## What the current strategy does

`short_call_strategy` answers: *if I sold an out-of-the-money call at the start of
every month, how often would the underlying stay below my strike, and what would
that have paid?*

For each month in the data:

1. **Entry** — take the underlying's close on the **first trading day** of the month.
2. **Target strike** — `entry_price × (1 + otm_pct)`. At `otm_pct = 0.10`, a
   spot of ₹63.85 gives a ₹70.24 target.
3. **Round to a real strike** — NSE only lists discrete strikes, so pick the
   **smallest listed CE strike at or above** the target. Rounding *up* is
   deliberate: rounding to nearest could land below spot and silently turn an
   OTM call into an ITM one.
4. **Premium** — the option's close on the **earliest trading day of the entry
   month** for that strike. This is what you'd collect for selling it.
5. **Outcome** — compare the underlying's close `month_offset` months later
   against the strike.
   - `check_price < strike` → `SUCCESS` (expired worthless, keep the premium)
   - otherwise → `FAIL` (assigned; intrinsic value is paid out)
6. **P&L** — `payoff = premium − max(0, check_price − strike)`, reported as
   `profit_pct = payoff / entry_price × 100`.

### Three things worth understanding about the output

**The payoff is the short call leg only — there is no stock position.** That
makes it a *naked* call write, not a covered call. If you hold the underlying,
a rally past your strike is largely offset by the gain on your shares; without
it, that rally is pure loss. Adding the stock leg would mean
`payoff += (check_price − entry_price)`, and it changes results dramatically.

**`FAIL` means "strike was breached", not "lost money".** A short call's
breakeven is `strike + premium`. If the premium collected exceeds how far the
underlying went in-the-money, the trade is still net positive despite the breach.

**Entry is deliberately the month's *first* trading day, not its last.** An
earlier version derived the strike from the month-end close while reading the
premium at month-start — lookahead bias that inflated the win rate by ~12
percentage points, because rally months got their strike set relative to the
*post-rally* price. `EntryUsesMonthStartNotMonthEnd` in the test suite locks this
down.

---

## Requirements

- **C++20** compiler (tested with MinGW-w64 GCC 15)
- **CMake** ≥ 3.20 and a generator (Ninja recommended)
- **Python 3.10+** with `pandas`, `requests`, `jugaad-data` for the data tools

```bash
pip install pandas requests jugaad-data
```

GoogleTest is fetched automatically by CMake — no manual install needed.

---

## Building

```bash
cmake -S . -B cmake-build-debug -G Ninja
cmake --build cmake-build-debug
```

Targets:

| Target | What it is |
| --- | --- |
| `backtest_core` | Static library: loaders, aggregation, strategies |
| `backtest_main` | Runnable driver (`src/main.cpp`) |
| `backtester_tests` | GoogleTest suite (11 tests) |

### Running the tests

Run from the **repository root** — the loader test reads a fixture by relative path.

```bash
./cmake-build-debug/tests/backtester_tests.exe
```

---

## Getting data

Two downloaders live in `tools/`. Both emit the same 16-column chain CSV schema
that `load_chain_csv` parses.

### `download_bhavcopies.py` — recommended

Pulls the daily NSE F&O bhavcopy, which carries **every strike and expiry in one
response**. Best coverage, no strike-range guessing.

```bash
cd tools && python download_bhavcopies.py
```

Handles both NSE format eras transparently:
- **Legacy** `fo{DDMONYYYY}.csv.zip` (2022 → Jul 2024) via `jugaad_data` (cached locally)
- **UDiff** `BhavCopy_NSE_FO_...` (Jul 2024 →) via direct HTTPS

Rows are filtered to front-month contracts (trade month = expiry month) so the
strategy's per-month premium lookup can't accidentally price a far-month contract.

### `download_data.py` — per-contract

Queries one `(expiry, strike, option_type)` at a time via `jugaad_data`, looping
monthly expiries and a strike band around spot. Slower and narrower than the
bhavcopy path, but useful for pulling a single contract's history. Also fetches
the underlying equity series, which the bhavcopy tool does not.

```bash
cd tools && python download_data.py
```

It auto-resolves NSE's monthly expiry day, which is **not** a fixed weekday —
SEBI moved stock F&O from last-Thursday to last-Tuesday around Sep 2025, and
either can shift a day earlier on an exchange holiday.

---

## Running a backtest

Edit `src/main.cpp` to point at your data and set parameters:

```cpp
backtester::run_short_call(
    "../data/sample/idfcfirstb_underlying_2022_2026.csv",  // underlying
    "../data/sample/idfcfirstb_ce_2022_2026.csv",          // option chain
    0.10,   // otm_pct: 10% OTM buffer
    2);     // month_offset
```

Then:

```bash
cd cmake-build-debug && ./backtest_main.exe
```

Results print to stdout and land in
`out/short_call/<underlying-stem>_otm_pct<N>_monthly_offset<M>.csv`:

```
entry_month,entry_price,strike,check_month,check_price,status,rounded_strike,premium,profit_pct
2022-01,49.65,54.615,2022-03,41.7,SUCCESS,55,0.55,1.10775
2023-05,63.85,70.235,2023-07,88.5,FAIL,71,0.35,-26.8598
```

### On `month_offset`

`month_offset` controls how far out the outcome is measured. Entry is always the
first trading day of month `i`; the check is the **last** trading day of month
`i + month_offset`.

- `0` — measure at the end of the entry month (≈ one monthly expiry cycle)
- `1` — measure a further month out
- `2` — two further months out

A row with `0,0,0` in the last three columns means no listed strike sat at or
above the target that month, so no trade was priced. Those rows are reported
honestly rather than back-filled with an estimate — check the strike ladder in
your chain CSV for that month before assuming it's a bug.

---

## Project layout

```
include/
  backtester/contract.hpp        Contract: underlying, strike, right, expiry, lot
  data/equity_loader.hpp         Underlying (EQ series) CSV loader
  data/loader.hpp                Option-chain CSV loader + ChainRow
  data/monthly_aggregate.hpp     Daily bars -> monthly first/last closes
  strategy/short_call.hpp        Strategy API, MonthOutcome, StrategyResult
  timer.hpp                      Scoped RAII timing helper
src/
  data/…                         Loader and aggregation implementations
  strategy/short_call.cpp        Strategy, P&L, CSV export
  main.cpp                       Driver
tests/                           GoogleTest suites
tools/                           Python data downloaders
data/sample/                     Sample CSVs and test fixtures
```

### Data schemas

**Underlying** (NSE equity bhavcopy format, 15 columns) — rows whose `SERIES` is
not `EQ` are skipped, which matters because NSE mixes NCD/bond series into the
same file under the same symbol.

**Option chain** (16 columns, strict order):

```
DATE, EXPIRY, OPTION TYPE, STRIKE PRICE, OPEN, HIGH, LOW, CLOSE, LTP,
SETTLE PRICE, TOTAL TRADED QUANTITY, MARKET LOT, PREMIUM VALUE,
OPEN INTEREST, CHANGE IN OI, SYMBOL
```

The loader skips rows with a blank `CLOSE` (NSE emits stub rows for strikes that
were listed but never traded and have no settle price) and treats other blank
numeric fields as `0`. Genuinely non-numeric values still throw, so a schema
change surfaces loudly instead of silently corrupting results.

---

## Roadmap

**Engine improvements**
- Compounded equity curves — the current `profit_pct` column sums as simple
  percentages, which assumes fixed sizing and no compounding
- Risk metrics: max drawdown, Sharpe, win/loss asymmetry, tail statistics
- Position sizing and lot-aware capital modelling
- Transaction costs, slippage, and STT/brokerage
- Parameter sweeps to compare buffers and offsets in one run
- Multi-symbol backtests rather than one underlying at a time

**Data layer**
- Cache option chains in a columnar format instead of re-parsing CSV
- Broader symbol coverage in the downloaders
- Data-quality checks for zero-volume strikes whose `CLOSE` is a marked-to-market
  settle price rather than a transactable premium

---

## A note on interpreting results

This is a research tool, not trading advice. Backtested results on short-option
strategies are especially easy to misread: a high win rate can coexist with
deeply negative expectancy, because the losses are rare and large while the wins
are frequent and small. Look at the cumulative P&L and the worst-case rows, not
just the SUCCESS/FAIL ratio.
