# options-backtest

A C++20 backtesting engine for NSE (India) equity options, with a Python data
pipeline and a localhost web UI.

It writes out-of-the-money calls against IDFCFIRSTB-style single-stock options
and answers three separate questions, deliberately kept apart:

1. **Did the option leg pay?** — premium collected against premium paid back.
2. **Did it beat owning the stock?** — the share leg, in rupees, against a
   buy-and-hold benchmark.
3. **What did the losing months have in common?** — entry conditions compared
   between winners and losers.

---

## What the strategy does

`short_call_strategy` answers: *if I sold an out-of-the-money call at the start of
every month, how often would the underlying stay below my strike, and what would
that have paid?*

For each entry month *i*, the strategy sells the contract expiring in month
*i + month_offset* and holds it to that contract's expiry:

1. **Entry price** — the underlying's close on the **first trading day** of month *i*.
2. **Target strike** — `entry_price × (1 + otm_pct)`. At `otm_pct = 0.10`, a
   spot of ₹63.85 gives a ₹70.24 target.
3. **Round to a real strike** — NSE only lists discrete strikes, so pick the
   **smallest listed CE strike at or above** the target. Rounding *up* is
   deliberate: rounding to nearest could land below spot and silently turn an
   OTM call into an ITM one.
4. **Require an entry-day quote** — the chosen strike must have been quoted on
   the same day the entry price came from. Without this, a contract first listed
   late in the month gets entered at a premium struck weeks after its own strike
   was chosen from day-one spot.
5. **Premium collected** — that contract's close on the first trading day of
   month *i*.
6. **Exit cost** (`check_premium`) — **the same contract**, at its close on the
   last trading day of its own expiry month. If it never traded in that month the
   exit settles at intrinsic value and the row is flagged
   `check_premium_is_market: false`.
7. **Verdict** — the underlying's close **on the contract's expiry date** against
   the strike: below is `HELD`, otherwise `BREACHED`.
8. **P&L** — the round trip: `payoff = premium − check_premium`, reported as
   `profit_pct = payoff / entry_price × 100`.

Both the exit and the verdict are anchored to **the contract's own expiry**, not
to a month boundary, because **NSE expiry falls before month end**. In July 2022
the 38 strike expired on the 28th with spot at 37.55 — worthless, last traded at
₹0.05 — and spot then ran to 41.95 by the 31st. Judging at month end would both
charge ₹3.95 of intrinsic value and call the strike breached, for a move the
contract was never exposed to.

Entry and exit are always **the same contract**. An earlier version priced the
exit from whatever traded at that strike in the check month, which at
`month_offset ≥ 1` was a different expiry entirely — selling a May contract and
buying back a June one. That produced large fictitious losses.

### Entry filters

| Filter | Flag | What it does |
| --- | --- | --- |
| OTM buffer | `--otm` | How far above spot the strike sits |
| Liquidity | `--min-volume N` | Requires the strike to have traded ≥ N contracts on the entry day; climbs to the next strike otherwise |
| Premium richness | `--yield-pct P` | Enters only when `premium / spot` clears the Pth percentile of the previous `--yield-lookback` cycles |

The premium filter's baseline is fed by **every priced candidate, not only
accepted cycles**. Feeding it the survivors would make it a record of what the
filter already liked, ratcheting the bar upward until nothing qualified. Cycles
arriving before the window fills are skipped rather than passed through, so the
filtered run is never credited with trades the rule did not approve.

### `priced` vs `entered`

Two different facts, kept separate because collapsing them hides why a run shrank:

- **`priced`** — a contract existed and could be priced. A `false` here is a
  statement about the *data*.
- **`entered`** — a position was actually opened. A `false` here is a statement
  about the *strategy's own rules*.

Every statistic about performance counts `entered`. Coverage reporting counts
`priced`. A skipped cycle carries a `skip_reason`: `no strike listed`,
`no baseline`, or `premium below threshold`.

### Three things worth understanding about the output

**`BREACHED` means the strike was crossed, not that the trade lost money.** A
short call's breakeven is `strike + premium`. If the premium collected exceeds
how far the underlying went in-the-money, the trade is still net positive despite
the breach.

**The option leg alone says nothing about beating the stock.** `profit_pct` is
premium in minus premium out. Whether writing calls was better than simply
holding the shares is the share leg's question — see below.

**Entry is deliberately the month's *first* trading day, not its last.** An
earlier version derived the strike from the month-end close while reading the
premium at month-start — lookahead bias that inflated the win rate by ~12
percentage points. `EntryUsesMonthStartNotMonthEnd` locks this down.

---

## The share leg

`simulate_covered_call` adds the stock, turning a naked write into a covered
call, and reports rupees rather than percentages of spot.

- **Cycles never overlap.** The option engine scans every entry month
  independently, so at `month_offset ≥ 1` it reports several positions open at
  once. One lot of stock covers one call, so the simulation takes its next entry
  only after the current one settles.
- **The holding is fixed.** You own N shares and write `floor(N / lot)` contracts.
  A lot revision changes how many calls the holding covers, not how much stock is
  owned — otherwise the position silently resizes away from the benchmark.
- **A breached strike means assignment.** At expiry the option has no time value
  left, so closing it and being assigned cost the same premium; the difference is
  the shares, which assignment takes away. `--no-rebuy` parks the proceeds in cash
  instead of buying back at the next entry.
- **Excess is a difference, not a ratio.** Both P&Ls can be negative, and 0.93×
  of a loss is an *outperformance* that a ratio reports backwards.

### Lot sizes are taken, never guessed

NSE revises lot sizes as a stock's price drifts — IDFCFIRSTB traded **7,500
shares per contract, revised to 9,275** during 2025 — and the legacy bhavcopy
format does not carry the column at all. Only 27 of 57 months in the sample state
a lot.

The multiplier scales every rupee figure, so a month with no stated lot is
**skipped and counted**, not filled in. `--lot-size N` supplies one explicitly;
cycles that use it are counted in `cycles_on_override` and the UI says so, because
an override otherwise silences the coverage warning by filling the gaps.

Turnover ÷ (contracts × spot) *does* approximate the lot, but measured against
months where NSE states one it runs **4–8% high** — turnover is struck at
intraday prices rather than the close — so it is not used as a default.

---

## Losing-month diagnostics

`server/diagnose.py` compares entry conditions between profitable and losing
cycles. Features are split in two, and only one half may produce findings:

- **Entry-time** — premium yield, strike distance, trailing volatility, run-in
  momentum, position in the trailing range, days to expiry, entry-day volume.
  Knowable before the trade. Actionable.
- **Outcome** — realised move over the cycle, finish versus strike. These
  separate winners from losers *perfectly* and predict nothing, because they are
  restatements of the result. Reported for understanding, never ranked.

Separation is measured with **Cliff's delta**, a rank statistic: the probability a
random losing cycle scores above a random winning one, minus the reverse. Used
instead of a difference of means because these samples are small, skewed, and
contain outliers. Effects below 0.147 are negligible.

**No p-values.** With dozens of cycles and seven features, a p-value would imply
confidence the sample cannot support, and testing several at once inflates any
such claim. Effect size and sample count are reported instead, and the result is
flagged outright when either group falls below 10 cycles.

The market context behind those features lives in `strategy/diagnostics.cpp`, and
every window **ends on the session before entry**. A test feeds two price series
that are identical up to entry and divergent after, asserting the features come
out equal — that is the lookahead guard.

---

## What the data has said so far

Findings from this codebase against IDFCFIRSTB, 2022–2026. Recorded because they
were expensive to establish and are easy to re-discover by accident.

**No entry condition separates losing months.** Across the 8 parameter
configurations with an adequate loser sample, every median Cliff's delta is ≤ 0.08
— all negligible. Trailing volatility points the same way in all 8 but at −0.07
the magnitude is nil.

**The premium filter's apparent edge is a stale-quote artefact.** At holds 1–2 it
looked spectacular, +3.9 per cycle against +0.8 unfiltered. Every filtered entry
at those holds — 18/18 and 17/17 — was on a contract that **never traded**.
Requiring real trades (`--min-volume 1`) collapses 5%/hold-1 from +3.937 to
+0.055 and leaves hold-2 with no qualifying cycles at all. An untraded far-dated
contract carries a frozen settlement mark, so screening for "high premium" against
those is a stale-mark detector, not an option-richness measure.

**At hold 0, where every entry is genuinely tradeable, the filter costs money** —
+0.147 → +0.084 and +0.230 → −0.120 per cycle.

**Far-dated contracts barely trade.** At `month_offset 0`, 55 of 55 entries are
real trades. At offset 1 it is 18 of 54; at offset 2, 4 of 53. Any result from a
longer hold that ignores volume is mostly fiction.

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
| `backtest_core` | Static library: loaders, aggregation, strategies, diagnostics, JSON output |
| `backtest_main` | Hardcoded-parameter driver (`src/main.cpp`) |
| `backtest_cli` | JSON-emitting CLI (`src/cli/main_cli.cpp`), built to `bin/` |
| `backtester_tests` | GoogleTest suite (62 tests) |

### Running the tests

Run from the **repository root** — the loader test reads a fixture by relative path.

```bash
./cmake-build-debug/tests/backtester_tests.exe
```

```bash
python -m pytest server/tests -q
```

---

## Web UI

A localhost web app for running backtests without editing `main.cpp`, plus
parameter sweeps across a grid. Start it from the repository root:

```bash
python -m uvicorn server.main:app --port 8000
```

Then open <http://127.0.0.1:8000>. It needs `backtest_cli` built first; if the
page says the engine wasn't found, `/api/health` reports every path it searched.

> **Do not add `--reload` on Windows.** The engine runs as a subprocess, and only
> the Proactor event loop can spawn one. The reloader installs the Selector loop
> *after* importing the app, so `create_subprocess_exec` raises
> `NotImplementedError` and every request returns 500. Setting the policy in
> `main.py` does not help — uvicorn overrides it. Restart by hand after editing
> the Python layer.

Architecture: browser → FastAPI → `backtest_cli` subprocess → `backtest_core`.
The engine emits JSON on stdout, Python derives the statistics, and the frontend
is plain HTML/JS with no build step, in a dark theme drawn from a
contrast-validated palette.

Install the server dependencies once:

```bash
pip install -e ".[dev]"
```

### Deploying (Vercel)

The engine is a compiled binary that shells out and spends about a second
re-parsing 95,000 chain rows per call. That does not belong behind a serverless
request, so the deployed site carries **no backend at all**: a grid of results is
computed locally and frozen to JSON.

```bash
cmake --build cmake-build-debug --target backtest_cli   # engine must exist
python tools/export_static.py                           # writes web/
```

That produces `web/` — the frontend plus `web/api/**.json` — which is committed,
because Vercel's Linux builders cannot run the engine to regenerate it. Point
Vercel at the repository; `vercel.json` sets `outputDirectory: web` with no build
step. Preview it exactly as deployed with:

```bash
python -m http.server 8001 --directory web
```

**One codebase, two modes.** On boot the frontend fetches `api/manifest.json`. If
it is there, static mode reads the precomputed files; if it is not, it POSTs to
the live server as before. Local development is unchanged.

A static build can only answer what it was built to answer, so static mode
replaces the free-text parameter inputs with **dropdowns of exactly what was
exported** — accepting a value and then failing to find a file would be worse
than not offering it. The grid is defined at the top of `tools/export_static.py`;
widen it and re-export to offer more. Share-leg controls are hidden because they
are fixed across the export.

### Endpoints

| Route | Purpose |
| --- | --- |
| `GET /api/health` | Engine path resolution and dataset count — check this first |
| `GET /api/datasets` | Symbols discovered in `data/sample/`, underlying paired with chain |
| `POST /api/backtest` | One combination: per-month rows, equity curve, share leg, diagnostics |
| `POST /api/sweep` | A grid of combinations, compared |

`POST /api/backtest` accepts `symbol`, `otm_pct`, `month_offset`,
`min_entry_volume`, `lots`, `lot_size_override`, `rebuy_after_assignment`,
`yield_percentile` and `yield_lookback`.

Clients name a symbol; the server resolves it to concrete paths from the
discovered list, so a request body never reaches a subprocess argument.

### CLI

The engine is usable directly, which is also the quickest way to tell whether a
problem is in the engine or the web layer:

```bash
./cmake-build-debug/bin/backtest_cli \
  --equity data/sample/idfcfirstb_underlying_2022_2026.csv \
  --chain  data/sample/idfcfirstb_ce_2022_2026.csv \
  --otm 0.05,0.10,0.15 --offset 0,1,2 --min-volume 1 --pretty
```

| Flag | Default | Purpose |
| --- | --- | --- |
| `--otm LIST` | `0.10` | OTM fractions, comma separated |
| `--offset LIST` | `1` | Holding periods in months |
| `--min-volume N` | `0` | Require N traded contracts on the entry day |
| `--yield-pct P` | `0` | Premium-richness percentile; 0 enters every cycle |
| `--yield-lookback N` | `12` | Cycles in the richness baseline |
| `--lots N` | `1` | Lots of stock held |
| `--lot-size N` | `0` | Multiplier for months the chain omits one |
| `--no-rebuy` | off | Stay in cash after assignment |
| `--max-runs N` | `200` | Cap on the parameter grid |
| `--pretty` / `--verbose` | off | Indent output / diagnostics to stderr |

Passing lists runs the whole grid in one process, so a sweep parses the chain CSV
once rather than once per combination. Stdout carries the JSON document and
nothing else; diagnostics go to stderr behind `--verbose`. Exit codes: `0` success,
`1` runtime error (with a JSON error body still on stdout), `2` bad arguments.

`meta.max_month_offset` reports the furthest hold the loaded chain can actually
open — the longest-dated expiry quoted on a month's first trading day. NSE lists
three serial expiries, so this is normally `2`; asking for more is not a failure
but an instrument that does not exist.

---

## Getting data

Two downloaders live in `tools/`. Both emit the same 16-column chain CSV schema
that `load_chain_csv` parses.

### `download_bhavcopies.py` — option chain

Pulls the daily NSE F&O bhavcopy, which carries **every strike and every expiry in
one response**. All expiries are kept: a multi-month hold sells a far-dated
contract and closes it at that contract's own expiry, which needs the far-dated
quotes present in the entry month.

```bash
cd tools && python download_bhavcopies.py
python download_bhavcopies.py --rebuild   # ignore what is on disk
```

**Incremental.** It reads the existing CSV, fetches only weekdays with no rows on
file, and merges. The file only ever grows, so a network blip can no longer delete
a day you already had. Holidays are re-probed each run — they look identical to
missing days without keeping a separate record, and the cost is a few dozen fast
requests.

A day that genuinely fails raises rather than returning `None`, so it cannot
masquerade as a holiday; remaining failures are listed at the end. On the legacy
archive a holiday reports itself as `BadZipFile` (an HTML error page instead of a
zip), which is treated as a closure, not an error.

Handles both NSE format eras transparently:
- **Legacy** `fo{DDMONYYYY}.csv.zip` (2022 → Jul 2024) via `jugaad_data` (cached locally)
- **UDiff** `BhavCopy_NSE_FO_...` (Jul 2024 →) via direct HTTPS

### `download_data.py` — underlying

Fetches the equity series, and can pull individual contracts by
`(expiry, strike, option_type)`.

```bash
cd tools && python download_data.py
```

Two NSE/`jugaad_data` quirks it works around, both of which silently corrupted
results before they were found:

**Dates arrive as IST midnight rendered in UTC** — `2022-01-02 18:30:00` *means*
Monday the 3rd. Truncating that naively shifts every bar back one trading day and
fills the file with Sundays. The fetch converts through `Asia/Kolkata`.

**Long ranges are chunked wrongly.** A single 2022→2026 request returned April
2026 twice and omitted May entirely, and asking for May alone also returned April
— both endpoints of that window are non-trading days. The fetch now runs one
request per calendar month, **verifies the rows are actually from the month
requested**, falls back to nudged endpoints when they are not, and refuses to
write a file with calendar gaps.

That last guard matters because `month_offset` counts months present in the data:
with May missing, an April entry at offset 1 was checked against June. The engine
now also verifies the calendar distance itself and skips any entry where it does
not match.

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
entry_month,entry_price,strike,check_month,check_price,status,rounded_strike,premium,check_premium,profit_pct
2022-01,49.65,54.615,2022-03,41.7,HELD,55,0.55,0.05,1.00705
2022-02,49.3,54.23,2022-04,39.5,HELD,55,0.35,0.05,0.608519
```

Two rates are printed, deliberately:

```
10% strike (1-month out)
  strike held, priced months only: 0.722222 (39/54)
  all months incl. unpriced:       0.732143 (41/56)
```

Folding unpriced months into one rate blends two different questions. An
`UNPRICED` row means no listed strike at or above the target existed for that
expiry — reported honestly rather than back-filled, and excluded from every
statistic.

### On `month_offset`

`month_offset` is the **holding period**: entering in month `i`, the strategy
sells the contract expiring in month `i + month_offset`.

- `0` — the front month: sell on the 1st, hold to that month's expiry
- `1` — sell the next month's contract, roughly a two-month hold
- `2` — the month after that

Every NSE trading day in the sample lists **exactly three expiries**, so offsets
beyond 2 correctly price nothing. Rows for a longer-dated expiry do appear late in
a month, once the front month has expired — but by then the strike was chosen
weeks earlier, so the entry-day rule rejects them.

---

## Project layout

```
include/
  backtester/contract.hpp        Contract: underlying, strike, right, expiry, lot
  data/equity_loader.hpp         Underlying (EQ series) CSV loader
  data/loader.hpp                Option-chain CSV loader + ChainRow
  data/monthly_aggregate.hpp     Daily bars -> monthly first/last closes
  strategy/short_call.hpp        Strategy API, MonthOutcome, YieldFilter
  strategy/position.hpp          Share leg: lot sizes, currency P&L, assignment
  strategy/diagnostics.hpp       Entry-time market context
  io/json_out.hpp                Result serialization
  timer.hpp                      Scoped RAII timing helper
src/
  data/…                         Loader and aggregation implementations
  strategy/short_call.cpp        Strategy, entry filters, P&L, CSV export
  strategy/position.cpp          Covered-call simulation and benchmark
  strategy/diagnostics.cpp       Trailing volatility, momentum, range position
  io/json_out.cpp                JSON emission (nlohmann confined to this TU)
  cli/main_cli.cpp               JSON CLI front end
  main.cpp                       Hardcoded-parameter driver
server/
  main.py                        FastAPI app and endpoints
  engine.py                      Engine discovery, subprocess, caching
  datasets.py                    Dataset discovery from data/sample/
  metrics.py                     Derived statistics, risk ratios (pure stdlib)
  diagnose.py                    Winners-vs-losers feature comparison
  static/                        Frontend: HTML, JS, vendored Chart.js
  tests/                         pytest suites
web/                            Generated static export (see Deploying)
third_party/nlohmann/            Vendored single-header JSON (MIT)
tests/                           GoogleTest suites
tools/                           Data downloaders and the static exporter
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

Two columns are not what their names suggest:

- **`PREMIUM VALUE` is notional turnover**, `contracts × lot × underlying price` —
  not premium paid. Dividing it by `contracts × spot` recovers the lot size.
- **`MARKET LOT` is only populated from Jul 2024**; the legacy format has no such
  column and the loader reads those rows as `0`.

The loader skips rows with a blank `CLOSE` (NSE emits stub rows for strikes that
were listed but never traded and have no settle price) and treats other blank
numeric fields as `0`. Genuinely non-numeric values still throw, so a schema
change surfaces loudly instead of silently corrupting results.

**About 70% of first-of-month quotes have zero traded volume.** Their `CLOSE` is a
carried settlement mark, not a price anyone transacted at — a contract can hold the
same frozen close across every day it is listed, so entry and exit agree exactly
and the month reports a P&L that was never available. `entry_volume` is always
reported; `--min-volume` decides whether to act on it.

---

## Roadmap

**Engine**
- Transaction costs, slippage, and STT/brokerage — every figure is currently gross
- Implied volatility by inverting Black-Scholes on the chain closes, which would
  allow delta-targeted strikes and an IV-versus-realised richness measure
- Expiry-cycle keying rather than calendar months, so entry can follow expiry
- Roll logic: DTE thresholds, delta breach, profit capture
- Compounded equity curves — `profit_pct` sums as simple percentages
- Persisting sweep results so runs can be compared across sessions

**Data layer**
- **Multi-symbol coverage.** The single largest limitation. One stock and ~55
  cycles per configuration is too thin to separate a pattern from noise; the
  downloaders already take any symbol.
- Dividend and corporate-action data — the underlying series is unadjusted
- A lot-size revision history, which would extend the rupee figures from 10
  months to the full period
- Cache option chains in a columnar format instead of re-parsing CSV

---

## A note on interpreting results

This is a research tool, not trading advice. Backtested results on short-option
strategies are especially easy to misread: a high win rate can coexist with deeply
negative expectancy, because the losses are rare and large while the wins are
frequent and small. Look at cumulative P&L and the worst rows, not the held/breached
ratio.

Two habits this codebase tries to enforce:

**Sample size before effect size.** A 55-cycle backtest can produce a large,
confident-looking effect that vanishes on a wider sample. The diagnostics flag
their own inadequacy rather than presenting a delta computed on three losing
months as a finding.

**Check liquidity before believing a number.** The most striking result this
engine has produced — a premium filter apparently tripling per-cycle returns —
was selecting contracts that never traded. If a configuration looks unusually
good, run it again with `--min-volume 1`.

---

## Licence

MIT — see [LICENSE](LICENSE). Vendored dependencies and the provenance of the
sample market data are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
