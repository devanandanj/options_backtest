"use strict";

const $ = (id) => document.getElementById(id);
const fmt = (v, dp = 2) => (v === null || v === undefined || Number.isNaN(v)) ? "—" : v.toFixed(dp);
const signClass = (v) => (v > 0 ? "pos" : v < 0 ? "neg" : "");

let equityChart = null;

// Read straight off the stylesheet so the chart and the heatmap can never drift
// from the theme; every value below is defined once in :root.
const css = (name, fallback) =>
  getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;

const THEME = {
  accent: () => css("--accent", "#3987e5"),
  text2: () => css("--text-2", "#c3c2b7"),
  muted: () => css("--muted", "#898781"),
  grid: () => css("--grid", "#2c2c2a"),
  axis: () => css("--axis", "#383835"),
  surface: () => css("--surface", "#1a1a19"),
};

// ── Static mode ──────────────────────────────────────────────────
//
// A deployed build has no engine behind it: tools/export_static.py precomputes a
// grid of results to files. If api/manifest.json is present we read those; if it
// is absent we are running against the live server and nothing changes.
//
// The grid is finite, so static mode replaces the free-text parameter inputs with
// dropdowns of exactly what was exported. Accepting a value and then failing to
// find a file would be worse than not offering it.
let STATIC = null;

async function detectStatic() {
  try {
    const res = await fetch("api/manifest.json", { cache: "no-store" });
    if (!res.ok) return null;
    const m = await res.json();
    return m && Array.isArray(m.runs) ? m : null;
  } catch {
    return null;   // no manifest, or not serving one: live server
  }
}

// Must match tools/export_static.py character for character.
const backtestKey = (sym, otm, off, mv, y) =>
  `${sym}__otm${(otm * 100).toFixed(1)}__h${off}__mv${Math.round(mv)}__y${Math.round(y)}`;
const sweepKey = (sym, mv, y) => `${sym}__mv${Math.round(mv)}__y${Math.round(y)}`;

async function fetchStatic(path, key, label) {
  const res = await fetch(`api/${path}/${key}.json`, { cache: "no-store" });
  if (!res.ok) {
    const err = new Error(`This combination was not part of the static export.`);
    err.detail = { error: err.message, available: [`asked for: ${label}`] };
    throw err;
  }
  return res.json();
}

// Swap a number input for a select over the exported values, preserving the id so
// every existing reader keeps working.
function constrainToOptions(id, values, format = (v) => v, selected = null) {
  const input = $(id);
  if (!input) return;
  const select = document.createElement("select");
  select.id = id;
  for (const v of values) {
    const opt = document.createElement("option");
    opt.value = String(v);
    opt.textContent = format(v);
    select.appendChild(opt);
  }
  select.value = String(selected !== null && values.includes(selected) ? selected : values[0]);

  // A unit suffix is positioned over the right edge of a text input, where a
  // select puts its dropdown arrow. The option labels carry the unit anyway
  // ("10%"), so drop the overlay rather than stack two glyphs on each other.
  const unit = input.parentElement?.querySelector(".unit");
  if (unit) unit.remove();

  input.replaceWith(select);
}

function applyStaticMode(m) {
  for (const [id, values, fmt, pick] of [
    ["single-otm", m.otm_pcts.map((p) => p * 100), (v) => `${v}%`, 10],
    ["single-offset", m.month_offsets, (v) => String(v), 0],
    ["single-minvol", m.min_volumes, (v) => (v ? `${v}+ traded` : "0 (any quote)"), 0],
    ["single-yieldpct", m.yield_percentiles, (v) => (v ? `${v}th pct` : "0 (no filter)"), 0],
  ]) {
    constrainToOptions(id, values, fmt, pick);
  }
  // Fixed across the export, so the controls would be decorative.
  const shareLeg = document.querySelectorAll(".controls-secondary")[0];
  if (shareLeg) shareLeg.hidden = true;
  const back = $("single-yieldback");
  if (back) back.closest("label")?.remove();

  constrainToOptions("sweep-minvol", m.min_volumes,
                     (v) => (v ? `${v}+ traded` : "0 (any quote)"), 0);
  constrainToOptions("sweep-yieldpct", m.yield_percentiles,
                     (v) => (v ? `${v}th pct` : "0 (no filter)"), 0);
  for (const id of ["sweep-otm", "sweep-offset"]) {
    const el = $(id);
    if (el) { el.readOnly = true; el.title = "fixed by the static export"; }
  }
}

async function api(path, body) {
  const opts = body
    ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }
    : {};
  const res = await fetch(path, opts);
  const payload = await res.json().catch(() => ({}));
  if (!res.ok) {
    const detail = payload.detail ?? payload;
    const err = new Error(detail.error || `Request failed (${res.status})`);
    err.detail = detail;
    err.status = res.status;
    throw err;
  }
  return payload;
}

function showError(el, err) {
  const d = err.detail || {};
  // Surface the engine's own diagnostics — stderr for a crash, stdout for the
  // "engine printed something that wasn't JSON" case.
  const extra = d.stderr_tail || d.stdout_head || (d.available ? `Available: ${d.available.join(", ")}` : "");
  el.innerHTML = `<strong>${err.message}</strong>${extra ? `<pre>${escapeHtml(extra)}</pre>` : ""}`;
  el.hidden = false;
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// ── Bootstrapping ────────────────────────────────────────────────

async function loadHealth() {
  const el = $("health");
  if (STATIC) {
    // No engine to report on. Say so plainly rather than showing a green dot for
    // a backend that is not there.
    el.textContent = `static export · ${STATIC.runs.length} precomputed runs`;
    el.classList.remove("bad");
    return;
  }
  try {
    const h = await api("/api/health");
    if (h.engine_found) {
      el.textContent = `engine ready · ${h.datasets_found} dataset(s)`;
      el.classList.remove("bad");
    } else {
      el.textContent = `engine not found — build it: ${h.build_hint}`;
      el.classList.add("bad");
    }
  } catch {
    el.textContent = "server unreachable";
    el.classList.add("bad");
  }
}

async function loadDatasets() {
  const { datasets } = STATIC
    ? await (await fetch("api/datasets.json", { cache: "no-store" })).json()
    : await api("/api/datasets");
  for (const selectId of ["single-symbol", "sweep-symbol"]) {
    const sel = $(selectId);
    sel.innerHTML = "";
    for (const d of datasets) {
      const opt = document.createElement("option");
      opt.value = d.symbol;
      opt.textContent = `${d.symbol.toUpperCase()} (${d.start_year}–${d.end_year})`
        + (d.has_chain ? "" : " — no chain data");
      sel.appendChild(opt);
    }
    if (!datasets.length) {
      sel.innerHTML = '<option value="">no datasets found</option>';
    }
  }
}

// ── Single run ───────────────────────────────────────────────────

// Returns true when the run priced nothing, having explained why.
//
// Asking for a hold the exchange does not list is a legitimate question with a
// real answer, not an error - so this is informational, and it names the actual
// ceiling the loaded chain supports rather than asserting a constant.
function renderEmptyState(data, requestedOffset) {
  const el = $("single-empty");
  const priced = data.summary?.priced_months ?? 0;
  if (priced > 0) {
    el.hidden = true;
    return false;
  }

  const max = data.meta?.max_month_offset;
  const beyondChain = Number.isInteger(max) && max >= 0 && requestedOffset > max;

  el.innerHTML = beyondChain
    ? `<h3>No contract exists ${requestedOffset} months out</h3>
       <p>The furthest hold this chain can open is <code>${max}</code> months, because that is
       the longest-dated expiry quoted on a month's first trading day &mdash; the only day an
       entry can be struck. NSE lists three serial expiries at a time (this month, next, and
       the one after), so a ${requestedOffset}-month hold has nothing to sell at entry.
       Contracts expiring ${requestedOffset} months out do appear later in a month, once the
       front month has expired, but by then the strike was chosen weeks earlier and the trade
       would not be the one you asked for. Try a hold of <code>${max}</code> or less.</p>`
    : `<h3>No month could be priced</h3>
       <p>Every entry month was skipped: no listed strike sat at or above the ${fmt(data.summary?.otm_pct * 100, 1)}%
       target on its entry day, or none met the minimum entry volume. Lower the OTM buffer or the
       volume floor and run it again.</p>`;
  el.hidden = false;
  return true;
}

// Rupees, with a sign, no decimals. Currency figures here run to six digits, where
// paise are noise; the sign is explicit because excess is read as a direction.
const money = (v) =>
  (v === null || v === undefined || Number.isNaN(v))
    ? "—"
    : `${v < 0 ? "−" : ""}₹${Math.abs(Math.round(v)).toLocaleString("en-IN")}`;

function renderPosition(data) {
  const p = data.position || {};
  const cycles = data.cycles || [];
  const section = $("position-section");

  if (p.not_applicable) {
    // The share leg does not apply to this instrument at all. Saying so beats
    // hiding the section, which would read as a rendering bug on an index.
    section.hidden = false;
    $("position-summary").innerHTML = "";
    $("cycles-table").querySelector("tbody").innerHTML = "";
    const banner = $("position-banner");
    banner.innerHTML = `<h3>No share leg for this underlying</h3>
      <p>${escapeHtml(p.not_applicable)}. The option leg above still applies &mdash;
      index options settle in cash against the index level, which is exactly what
      <em>check price</em> measures.</p>`;
    banner.hidden = false;
    return;
  }

  if (!cycles.length) {
    // No lot size was ever known, so no rupee figure can be honest.
    section.hidden = true;
    return;
  }
  section.hidden = false;

  const banner = $("position-banner");
  const skipped = (p.skipped_unknown_lot || 0) + (p.skipped_under_one_lot || 0);
  const assumed = p.cycles_on_override || 0;

  if (assumed > 0) {
    // An override silences the coverage warning by filling the gaps, so it has to
    // raise one of its own: these rupees rest on a multiplier nobody published.
    banner.innerHTML = `
      <h3>${assumed} of ${p.cycles_run} cycles use an assumed lot size</h3>
      <p>NSE does not state a lot size for those months, so the override you set stands in for
      it. The multiplier scales every rupee figure here in proportion, and measuring it from
      turnover runs 4&ndash;8% high against months where NSE does state one &mdash; so treat the
      totals as resting on that assumption. Set the override back to <code>0</code> to see only
      the months with a published lot.</p>`;
    banner.hidden = false;
  } else if (skipped > 0) {
    // The percentage metrics span the whole period and these do not. Saying so is
    // the difference between a caveat and a misleading number.
    const parts = [];
    if (p.skipped_unknown_lot) {
      parts.push(`<strong>${p.skipped_unknown_lot}</strong> because NSE's lot size is not in the
        data for those months &mdash; the multiplier scales every rupee figure, so it is left
        unknown rather than guessed`);
    }
    if (p.skipped_under_one_lot) {
      parts.push(`<strong>${p.skipped_under_one_lot}</strong> because a raised lot size grew past
        the holding, which can no longer cover a single contract`);
    }
    banner.innerHTML = `
      <h3>These rupee figures cover ${p.covered_months} of ${data.summary.total_months} months</h3>
      <p>${p.first_month} to ${p.last_month} only. ${skipped} month(s) were skipped:
      ${parts.join("; ")}. The percentages above span the full period, so the two sections are
      not directly comparable. Set a <em>lot size override</em> to extend the coverage, bearing
      in mind the figures then rest on the lot you assumed.</p>`;
    banner.hidden = false;
  } else {
    banner.hidden = true;
  }

  const ahead = p.excess_pnl >= 0;
  const cards = [
    ["Excess vs buy &amp; hold", money(p.excess_pnl),
      ahead ? "ahead of simply holding" : "behind simply holding", signClass(p.excess_pnl)],
    ["Strategy P&amp;L", money(p.end_pnl), `${p.shares.toLocaleString("en-IN")} shares`, signClass(p.end_pnl)],
    ["Buy &amp; hold P&amp;L", money(p.benchmark_pnl), "same shares, just held", signClass(p.benchmark_pnl)],
    ["Premium collected", money(p.premium_collected), `over ${p.cycles_run} cycles`, "pos"],
    ["Assignments", String(p.assignments), `of ${p.cycles_run} cycles`, ""],
    ["Max drawdown", money(p.max_drawdown), "peak to trough", "neg"],
  ];
  $("position-summary").innerHTML = cards.map(([label, value, sub, cls], i) => `
    <div class="stat${i === 0 ? " hero" : ""}">
      <div class="label">${label}</div>
      <div class="value ${cls}">${value}</div>
      ${sub ? `<div class="sub">${sub}</div>` : ""}
    </div>`).join("");

  $("cycles-table").querySelector("tbody").innerHTML = cycles.map((c) => `
    <tr>
      <td>${c.entry_month}</td><td>${c.check_month}</td>
      <td>${(c.shares || 0).toLocaleString("en-IN")}</td>
      <td>${(c.lot_size || 0).toLocaleString("en-IN")}</td>
      <td>${c.contracts}</td>
      <td>${fmt(c.entry_spot)}</td><td>${fmt(c.strike)}</td><td>${fmt(c.exit_spot)}</td>
      <td class="pos">${money(c.premium_collected)}</td>
      <td>${c.assigned
            ? '<span class="badge assigned">called away</span>'
            : '<span class="badge held">kept</span>'}</td>
      <td class="${signClass(c.pnl)}">${money(c.pnl)}</td>
    </tr>`).join("");
}

// Cliff's delta runs -1..+1; the track maps that onto its full width with zero at
// the centre. NEGLIGIBLE mirrors the threshold the server uses, so the drawn band
// and the "negligible" label can never disagree.
const NEGLIGIBLE = 0.147;

function deltaBar(delta) {
  const half = Math.min(Math.abs(delta), 1) * 50;
  const left = delta >= 0 ? 50 : 50 - half;
  const band = NEGLIGIBLE * 50;
  const muted = Math.abs(delta) < NEGLIGIBLE ? " negligible" : "";
  return `<div class="diag-track">
    <div class="diag-band" style="left:${50 - band}%;width:${band * 2}%"></div>
    <div class="diag-mid"></div>
    <div class="diag-bar${muted}" style="left:${left}%;width:${half}%"></div>
  </div>`;
}

function renderDiagnostics(data) {
  const d = data.diagnostics || {};
  const section = $("diag-section");
  const findings = d.findings || [];

  if (!findings.length) {
    section.hidden = true;
    return;
  }
  section.hidden = false;

  const banner = $("diag-banner");
  const strongest = Math.max(...findings.map((f) => Math.abs(f.delta)));
  if (!d.sample_adequate) {
    // Leading with this, because a table of deltas computed on a handful of losing
    // months looks exactly as authoritative as one computed on hundreds.
    banner.innerHTML = `
      <h3>Too few losing months to conclude anything</h3>
      <p>${d.losing_cycles} losing and ${d.winning_cycles} winning cycles; this needs at least
      <code>${d.min_group}</code> a side before a difference means more than chance. The figures
      below are computed anyway so you can see them, but treat any large-looking effect as an
      artefact of the sample. A tighter OTM buffer or a longer hold produces more losing months
      to compare.</p>`;
    banner.hidden = false;
  } else if (strongest < NEGLIGIBLE) {
    // A null result is a result, and it should be stated rather than left for the
    // reader to infer from seven short bars.
    banner.innerHTML = `
      <h3>Nothing separates the losing months</h3>
      <p>Across ${d.losing_cycles} losing and ${d.winning_cycles} winning cycles, no entry
      condition reaches even a negligible effect &mdash; every bar below sits inside the shaded
      band. On this data, losing months were not distinguishable in advance by any of the
      ${d.features_tested} conditions tested.</p>`;
    banner.hidden = false;
  } else {
    banner.hidden = true;
  }

  $("diag-bars").innerHTML = findings.map((f) => `
    <div class="diag-row">
      <div class="diag-name">${f.label}</div>
      ${deltaBar(f.delta)}
      <div class="diag-value">${f.delta >= 0 ? "+" : "−"}${Math.abs(f.delta).toFixed(2)}</div>
    </div>`).join("");

  $("diag-table").querySelector("tbody").innerHTML = findings.map((f) => `
    <tr>
      <td>${f.label}</td>
      <td>${fmt(f.losers_median)}</td>
      <td>${fmt(f.winners_median)}</td>
      <td>${f.delta >= 0 ? "+" : "−"}${Math.abs(f.delta).toFixed(2)}</td>
      <td><span class="mag ${f.magnitude}">${f.magnitude}</span></td>
      <td>${f.direction}</td>
      <td>${f.losers_n}/${f.winners_n}</td>
    </tr>`).join("");

  $("diag-outcome-table").querySelector("tbody").innerHTML =
    (d.outcome_context || []).map((f) => `
      <tr>
        <td>${f.label}</td>
        <td>${fmt(f.losers_median)}</td>
        <td>${fmt(f.winners_median)}</td>
        <td>${f.delta >= 0 ? "+" : "−"}${Math.abs(f.delta).toFixed(2)}</td>
      </tr>`).join("");
}

// When the premium filter is on, the entered count alone looks like the data ran
// thin. Naming what the rule turned away keeps the shrinkage attributable.
function coverageSub(s) {
  const declined = s.skipped_low_yield || 0;
  const nobase = s.skipped_no_baseline || 0;
  const base = `${s.priced_months} of ${s.total_months} months`;
  if (!declined && !nobase) return base;
  const parts = [];
  if (declined) parts.push(`${declined} below threshold`);
  if (nobase) parts.push(`${nobase} no baseline`);
  return `${base} · ${parts.join(", ")}`;
}

function renderSummary(s) {
  const cards = [
    ["Cumulative P&L", `${fmt(s.cumulative_pct)}%`, "sum of monthly P&L", signClass(s.cumulative_pct)],
    ["Max drawdown", `${fmt(s.max_drawdown_pct)}%`, "peak to trough, points", "neg"],
    ["Strike held", `${fmt(s.strike_held_pct, 1)}%`, `${s.priced_months} priced months`, ""],
    ["Coverage", `${fmt(s.coverage_pct, 1)}%`, coverageSub(s), ""],
    ["Really traded", `${fmt(s.traded_pct, 1)}%`, `${s.traded_months} of ${s.priced_months} priced`, ""],
    ["Avg win", `${fmt(s.avg_win_pct)}%`, `${s.profitable_months} months`, "pos"],
    ["Avg loss", `${fmt(s.avg_loss_pct)}%`, `${s.losing_months} months`, "neg"],
    ["Best month", `${fmt(s.best_month_pct)}%`, "", "pos"],
    ["Worst month", `${fmt(s.worst_month_pct)}%`, "", "neg"],
  ];
  // The first card is the headline. Giving every metric the same weight makes a
  // wall of numbers with no entry point, so it renders larger and wider.
  $("single-summary").innerHTML = cards.map(([label, value, sub, cls], i) => `
    <div class="stat${i === 0 ? " hero" : ""}">
      <div class="label">${label}</div>
      <div class="value ${cls}">${value}</div>
      ${sub ? `<div class="sub">${sub}</div>` : ""}
    </div>`).join("");
}

function renderEquityChart(curve) {
  const ctx = $("equity-chart").getContext("2d");
  if (equityChart) equityChart.destroy();
  equityChart = new Chart(ctx, {
    type: "line",
    data: {
      labels: curve.map((p) => p.month),
      datasets: [{
        label: "Cumulative P&L (percentage points)",
        data: curve.map((p) => p.cumulative_pct),
        borderColor: THEME.accent(),
        backgroundColor: "rgba(57,135,229,.13)",
        fill: true,
        pointRadius: 0,
        // Hover target is larger than the mark, per the interaction spec.
        pointHoverRadius: 5,
        pointHoverBorderWidth: 2,
        pointHoverBackgroundColor: THEME.accent(),
        pointHoverBorderColor: THEME.surface(),
        borderWidth: 2,
        tension: 0.15,
      }],
    },
    options: {
      responsive: true,
      // The container fixes the height, so the canvas must not also derive one from
      // its aspect ratio - otherwise it grows instead of fitting the space given.
      maintainAspectRatio: false,
      interaction: { mode: "index", intersect: false },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: "#000",
          borderColor: css("--border-strong", "rgba(255,255,255,.18)"),
          borderWidth: 1,
          padding: 10,
          titleColor: "#fff",
          bodyColor: THEME.text2(),
          displayColors: false,
          callbacks: {
            afterLabel: (item) => `month: ${fmt(curve[item.dataIndex].profit_pct)}%`,
          },
        },
      },
      scales: {
        y: {
          title: { display: true, text: "cumulative %", color: THEME.muted() },
          grid: { color: THEME.grid(), drawTicks: false },
          border: { display: false },
          ticks: { color: THEME.muted(), padding: 8 },
        },
        x: {
          grid: { display: false },
          border: { color: THEME.axis() },
          ticks: { color: THEME.muted(), maxTicksLimit: 14, maxRotation: 0 },
        },
      },
    },
  });
}

// The asterisk means "this exit was derived from intrinsic value, not a real quote".
// It must only appear when there IS a value to qualify — an absent field means the
// engine predates the field, and marking that as an intrinsic fallback would be a lie.
// Entry-day volume in the sold strike. Zero means the premium came from a carried
// settlement mark on a contract nobody traded, so the month's P&L was never
// actually available - flagged rather than hidden, since the row is still priced.
function entryVolumeCell(o) {
  const v = o.entry_volume;
  if (v === null || v === undefined) return '<td>' + '—' + '</td>';
  if (v > 0) return `<td>${v.toLocaleString()}</td>`;
  return '<td class="untraded" title="listed but never traded on the entry day - '
       + 'this premium is a settlement mark, not a tradeable price">0</td>';
}

function exitPremiumCell(o) {
  if (o.check_premium === null || o.check_premium === undefined) {
    return '<td title="engine did not report an exit premium">—</td>';
  }
  if (o.check_premium_is_market) {
    return `<td>${fmt(o.check_premium)}</td>`;
  }
  return `<td title="no quote that month — intrinsic value used">${fmt(o.check_premium)}*</td>`;
}

function renderOutcomes(outcomes) {
  const body = $("outcomes-table").querySelector("tbody");
  body.innerHTML = outcomes.map((o) => {
    // An unpriced month never traded. Showing its zeros as numbers would read as
    // a break-even result, so every derived field renders as an em dash instead.
    if (!o.priced) {
      return `<tr class="unpriced">
        <td>${o.entry_month}</td><td>${fmt(o.entry_price)}</td><td>${fmt(o.strike)}</td>
        <td>—</td><td>—</td><td>—</td>
        <td>${o.check_month}</td><td>${fmt(o.check_price)}</td><td>—</td>
        <td>—</td>
        <td><span class="badge none">no strike listed</span></td><td>—</td>
      </tr>`;
    }
    // Priced but declined: the contract was there and the rule passed it over. It
    // is greyed like an unpriced row, but says so in its own words - the premium
    // and yield stay visible, since seeing what was turned down is the point.
    if (!o.entered) {
      return `<tr class="unpriced">
        <td>${o.entry_month}</td><td>${fmt(o.entry_price)}</td><td>${fmt(o.strike)}</td>
        <td>${fmt(o.rounded_strike)}</td><td>${fmt(o.premium)}</td>
        <td title="needed ${fmt(o.yield_threshold_pct)}%">${fmt(o.premium_yield_pct)}</td>
        <td>${o.check_month}</td><td>${fmt(o.check_price)}</td><td>—</td>
        <td>—</td>
        <td><span class="badge none">${escapeHtml(o.skip_reason || "not entered")}</span></td>
        <td>—</td>
      </tr>`;
    }
    return `<tr>
      <td>${o.entry_month}</td><td>${fmt(o.entry_price)}</td><td>${fmt(o.strike)}</td>
      <td>${fmt(o.rounded_strike)}</td><td>${fmt(o.premium)}</td>
      <td>${fmt(o.premium_yield_pct)}</td>
      <td>${o.check_month}</td><td>${fmt(o.check_price)}</td>
      ${exitPremiumCell(o)}
      ${entryVolumeCell(o)}
      <td><span class="badge ${o.held ? "held" : "breach"}">${o.held ? "held" : "breached"}</span></td>
      <td class="${signClass(o.profit_pct)}">${fmt(o.profit_pct)}</td>
    </tr>`;
  }).join("");
}

async function runSingle(evt) {
  evt.preventDefault();
  const btn = evt.target.querySelector("button");
  const errEl = $("single-error");
  btn.disabled = true;
  errEl.hidden = true;

  const symbol = $("single-symbol").value;
  const otm = Number($("single-otm").value) / 100;
  const offset = Number($("single-offset").value);
  const minvol = Number($("single-minvol").value) || 0;
  const yieldPct = Number($("single-yieldpct").value) || 0;

  try {
    const data = STATIC
      ? await fetchStatic("backtest", backtestKey(symbol, otm, offset, minvol, yieldPct),
                          `${symbol} ${otm * 100}% OTM, hold ${offset}`)
      : await api("/api/backtest", {
          symbol,
          otm_pct: otm,
          month_offset: offset,
          min_entry_volume: minvol,
          lots: Number($("single-lots").value) || 1,
          lot_size_override: Number($("single-lotsize").value) || 0,
          rebuy_after_assignment: $("single-rebuy").checked,
          yield_percentile: yieldPct,
          yield_lookback: Number($("single-yieldback").value) || 12,
        });
    // Unhide first. Chart.js measures its container when the chart is constructed,
    // and with maintainAspectRatio off a hidden container measures zero - which it
    // then keeps, drawing nothing, until some later resize it may never receive.
    $("single-results").hidden = false;
    const nothingPriced = renderEmptyState(data, Number($("single-offset").value));
    // Summary and curve are meaningless with nothing priced, and a grid of 0.00%
    // beside an empty chart is the thing that reads as a bug. The table stays: it
    // still shows which months were skipped and why.
    $("single-summary").hidden = nothingPriced;
    $("single-result-head").hidden = nothingPriced;
    document.querySelector(".chart-wrap").hidden = nothingPriced;
    if (!nothingPriced) {
      renderSummary(data.summary);
      renderEquityChart(data.equity_curve);
    }
    renderPosition(nothingPriced ? { position: {}, cycles: [] } : data);
    renderDiagnostics(nothingPriced ? { diagnostics: {} } : data);
    renderOutcomes(data.outcomes);
  } catch (err) {
    showError(errEl, err);
    $("single-results").hidden = true;
  } finally {
    btn.disabled = false;
  }
}

// ── Sweep ────────────────────────────────────────────────────────

function parseList(text) {
  return text.split(",").map((s) => Number(s.trim())).filter((n) => !Number.isNaN(n));
}

// Diverging blue<->red around a neutral midpoint, not green<->red: under
// deuteranopia that green and that red measure dE 4.1 apart, so the classic
// finance pairing would leave the grid unreadable for the people it fails.
// Sign is still spelled out by the number printed in every cell.
const HEAT_GAIN = [54, 124, 209];   // blue pole
const HEAT_LOSS = [190, 56, 56];    // red pole

function mixToSurface(pole, t) {
  // Cap the mix so white cell text keeps >= 4.5:1 against the darkest fill.
  const k = Math.min(0.82, 0.10 + 0.72 * t);
  const base = [26, 26, 25];        // --surface
  const ch = base.map((b, i) => Math.round(b + (pole[i] - b) * k));
  return `rgb(${ch.join(",")})`;
}

function heatColor(value, min, max) {
  if (value >= 0) return mixToSurface(HEAT_GAIN, max > 0 ? value / max : 0);
  return mixToSurface(HEAT_LOSS, min < 0 ? value / min : 0);
}

function renderHeatmap(data) {
  const { otm_pcts, month_offsets, cells } = data;
  const byKey = new Map(cells.map((c) => [`${c.otm_pct}|${c.month_offset}`, c]));
  const values = cells.map((c) => c.cumulative_pct);
  const min = Math.min(0, ...values);
  const max = Math.max(0, ...values);
  const bestKey = `${data.best.summary.otm_pct}|${data.best.summary.month_offset}`;

  const legendMin = $("heat-min");
  const legendMax = $("heat-max");
  if (legendMin) legendMin.textContent = `${fmt(min, 1)}%`;
  if (legendMax) legendMax.textContent = `+${fmt(max, 1)}%`;

  const grid = $("heatmap");
  grid.style.gridTemplateColumns = `auto repeat(${month_offsets.length}, 1fr)`;

  const parts = ['<div class="heat-cell header">otm \\ offset</div>'];
  for (const off of month_offsets) parts.push(`<div class="heat-cell header">${off}</div>`);

  for (const otm of otm_pcts) {
    parts.push(`<div class="heat-cell header">${(otm * 100).toFixed(1)}%</div>`);
    for (const off of month_offsets) {
      const cell = byKey.get(`${otm}|${off}`);
      if (!cell) { parts.push('<div class="heat-cell">—</div>'); continue; }
      // Zero priced months is "no such trade", not "broke even" - shading it on the
      // diverging ramp would rank it against combinations that actually traded.
      if (!cell.priced_months) {
        parts.push(`<div class="heat-cell none" title="${escapeHtml(
          `OTM ${(cell.otm_pct * 100).toFixed(1)}%, offset ${cell.month_offset}
not listed: no contract could be opened that far out`)}">n/a</div>`);
        continue;
      }
      const key = `${cell.otm_pct}|${cell.month_offset}`;
      const title = [
        `OTM ${(cell.otm_pct * 100).toFixed(1)}%, offset ${cell.month_offset}`,
        `cumulative ${fmt(cell.cumulative_pct)}%`,
        `max drawdown ${fmt(cell.max_drawdown_pct)}%`,
        `strike held ${fmt(cell.strike_held_pct, 1)}%`,
        `coverage ${cell.priced_months}/${cell.total_months}`,
        `really traded ${cell.traded_months}/${cell.priced_months}`,
      ].join("\n");
      parts.push(`<div class="heat-cell ${key === bestKey ? "best" : ""}"
        style="background:${heatColor(cell.cumulative_pct, min, max)}"
        title="${escapeHtml(title)}">${fmt(cell.cumulative_pct, 1)}</div>`);
    }
  }
  grid.innerHTML = parts.join("");
}

function renderSweepTable(cells) {
  const body = $("sweep-table").querySelector("tbody");
  const sorted = [...cells].sort((a, b) => b.cumulative_pct - a.cumulative_pct);
  body.innerHTML = sorted.map((c) => `
    <tr>
      <td>${(c.otm_pct * 100).toFixed(1)}</td>
      <td>${c.month_offset}</td>
      <td class="${signClass(c.cumulative_pct)}">${fmt(c.cumulative_pct)}</td>
      <td class="neg">${fmt(c.max_drawdown_pct)}</td>
      <td>${fmt(c.strike_held_pct, 1)}</td>
      <td>${c.priced_months}/${c.total_months}</td>
      <td>${c.traded_months}/${c.priced_months}</td>
      <td class="pos">${fmt(c.avg_win_pct)}</td>
      <td class="neg">${fmt(c.avg_loss_pct)}</td>
      <td class="neg">${fmt(c.worst_month_pct)}</td>
    </tr>`).join("");
}

async function runSweep(evt) {
  evt.preventDefault();
  const btn = evt.target.querySelector("button");
  const errEl = $("sweep-error");
  btn.disabled = true;
  errEl.hidden = true;

  const sweepSymbol = $("sweep-symbol").value;
  const sweepMinvol = Number($("sweep-minvol").value) || 0;
  const sweepYield = Number(($("sweep-yieldpct") || {}).value) || 0;

  try {
    const data = STATIC
      ? await fetchStatic("sweep", sweepKey(sweepSymbol, sweepMinvol, sweepYield),
                          `${sweepSymbol} sweep`)
      : await api("/api/sweep", {
          symbol: sweepSymbol,
          otm_pcts: parseList($("sweep-otm").value).map((n) => n / 100),
          month_offsets: parseList($("sweep-offset").value),
          min_entry_volume: sweepMinvol,
        });
    renderHeatmap(data);
    renderSweepTable(data.cells);
    $("sweep-results").hidden = false;
  } catch (err) {
    showError(errEl, err);
    $("sweep-results").hidden = true;
  } finally {
    btn.disabled = false;
  }
}

// ── Init ─────────────────────────────────────────────────────────

document.querySelectorAll(".tab").forEach((tab) => {
  tab.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((t) => t.classList.toggle("active", t === tab));
    document.querySelectorAll(".panel").forEach((p) => { p.hidden = p.id !== tab.dataset.panel; });
  });
});

$("single-form").addEventListener("submit", runSingle);
$("sweep-form").addEventListener("submit", runSweep);

// Static detection has to finish before anything reads STATIC, so the boot
// sequence is a chain rather than two loose calls.
detectStatic().then((manifest) => {
  STATIC = manifest;
  if (STATIC) {
    document.body.classList.add("static-mode");
    applyStaticMode(STATIC);
  }
  loadHealth();
  return loadDatasets();
}).catch((err) => showError($("single-error"), err));
