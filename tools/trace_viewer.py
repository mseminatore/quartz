#!/usr/bin/env python3
"""trace_viewer.py — View a Chrome Trace Event Format JSON file in any browser.

Reads a trace.json produced by tools/trace_to_chrome.py and writes a
self-contained HTML file with an interactive canvas-based timeline viewer.
No Chrome required — works in Firefox, Safari, or any modern browser.

Usage:
    python3 tools/trace_viewer.py trace.json
    python3 tools/trace_viewer.py trace.json -o out.html
    python3 tools/trace_viewer.py trace.json --open
"""

import argparse
import json
import os
import sys
import webbrowser

# ---------------------------------------------------------------------------
# HTML / JS template.
# Placeholders replaced at generation time (NOT Python format strings, so
# curly braces in CSS / JS are left as-is):
#   __TRACE_DATA__  → JSON-serialised trace object
#   __FILENAME__    → basename of the input file
# ---------------------------------------------------------------------------
_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>Trace — __FILENAME__</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%;overflow:hidden;font-family:monospace;font-size:12px;background:#fff;color:#222}
#toolbar{display:flex;align-items:center;gap:6px;padding:4px 10px;background:#eee;border-bottom:1px solid #ccc;flex-shrink:0;user-select:none;flex-wrap:wrap}
#toolbar h1{font-size:13px;font-weight:bold;margin-right:4px}
button{font-size:11px;padding:2px 8px;cursor:pointer;border:1px solid #aaa;background:#fff;border-radius:3px}
button:hover{background:#ddd}
#status{font-size:10px;color:#777;margin-left:auto}
#outer{display:flex;flex-direction:column;height:calc(100vh - 29px)}
#wrap{flex:1;position:relative;overflow:hidden}
canvas{display:block}
#tip{position:absolute;display:none;pointer-events:none;background:rgba(0,0,0,.82);color:#fff;font-size:11px;padding:4px 8px;border-radius:4px;white-space:pre;z-index:9;max-width:380px;word-break:break-all}
#det{display:none;border-top:1px solid #ccc;background:#fafafa;padding:6px 10px;flex-shrink:0;max-height:160px;overflow-y:auto;font-size:11px}
#det h3{font-size:12px;font-weight:bold;margin-bottom:4px}
#det table{border-collapse:collapse;width:100%}
#det td{padding:2px 4px;border-bottom:1px solid #eee;vertical-align:top}
#det td:first-child{color:#888;width:110px;white-space:nowrap}
#det .x{float:right;cursor:pointer;font-size:14px;color:#999;margin-left:8px}
#det .x:hover{color:#333}
</style></head>
<body>
<div id="toolbar">
  <h1>Trace — __FILENAME__</h1>
  <button id="bFit">Fit (F)</button>
  <button id="bZI">Zoom In (+)</button>
  <button id="bZO">Zoom Out (−)</button>
  <span id="status"></span>
</div>
<div id="outer">
  <div id="wrap"><canvas id="cv"></canvas><div id="tip"></div></div>
  <div id="det">
    <span class="x" id="detX">✕</span>
    <h3 id="detH"></h3>
    <table id="detT"></table>
  </div>
</div>
<script>
'use strict';
const TRACE = __TRACE_DATA__;

/* ── Layout constants ── */
const RH = 36;   // ruler height (px)
const LW = 185;  // label column width (px)
const LH = 28;   // lane height (px)
const LP = 4;    // bar top/bottom padding inside lane (px)
const BH = LH - LP * 2;  // bar height (px)

/* ── cname colour palette matching chrome://tracing ── */
const CN = {
  good:'#4CAF50', bad:'#E53935', terrible:'#B71C1C',
  black:'#333', grey:'#9E9E9E', white:'#eee',
  olive:'#9E9D24', yellow:'#F9A825', orange:'#E65100',
  rail_response:'#AB47BC', rail_animation:'#66BB6A',
  rail_idle:'#B0BEC5', rail_load:'#FF9800', startup:'#FFEE58',
  cq_build_attempt_runnning:'#29B6F6',
  cq_build_passed:'#26C6DA', cq_build_failed:'#EF5350',
  cq_build_abandoned:'#90A4AE', generic_work:'#42A5F5',
};
const C_RUN = '#42A5F5';  // default colour for "running" slices
const C_DEF = '#78909C';  // default for unlabelled IPC events

/* ── Process trace data into structured form ── */
function processTrace(trace) {
  const evs = trace.traceEvents || [];
  const tmeta = {};  // `${pid}:${tid}` → {name, sort}

  /* First pass: collect metadata events */
  for (const e of evs) {
    if (e.ph !== 'M') continue;
    const k = `${e.pid}:${e.tid}`;
    if (!tmeta[k]) tmeta[k] = {name: '', sort: e.tid};
    if (e.name === 'thread_name' && e.args && e.args.name)
      tmeta[k].name = e.args.name;
    if (e.name === 'thread_sort_index' && e.args && 'sort_index' in e.args)
      tmeta[k].sort = e.args.sort_index;
  }

  /* Second pass: pair B/E events; collect X events */
  const pendB = {};  // `${pid}:${tid}` → stack of open B events
  const all = [];    // all complete (drawable) events
  for (const e of evs) {
    if (e.ph === 'M') continue;
    const k = `${e.pid}:${e.tid}`;
    if (!tmeta[k]) tmeta[k] = {name: String(e.tid), sort: e.tid};
    if (e.ph === 'X') {
      all.push({...e, _e: e.ts + (e.dur || 0)});
    } else if (e.ph === 'B') {
      (pendB[k] = pendB[k] || []).push(e);
    } else if (e.ph === 'E') {
      const stk = pendB[k];
      if (stk && stk.length) {
        const b = stk.pop();
        all.push({...b, dur: e.ts - b.ts, _e: e.ts,
                  args: {...(b.args || {}), ...(e.args || {})}});
      }
    }
  }

  /* Build sorted thread list */
  const threads = Object.entries(tmeta).map(([k, m]) => {
    const [pid, tid] = k.split(':').map(Number);
    return {pid, tid, k, name: m.name || String(tid), sort: m.sort};
  }).sort((a, b) => a.sort - b.sort || a.tid - b.tid);

  /* Group events by thread key */
  const byT = {};
  for (const e of all) {
    const k = `${e.pid}:${e.tid}`;
    (byT[k] = byT[k] || []).push(e);
  }

  /* Global time range */
  let mn = Infinity, mx = -Infinity;
  for (const e of all) {
    if (e.ts < mn) mn = e.ts;
    if (e._e > mx) mx = e._e;
  }
  if (!isFinite(mn)) { mn = 0; mx = 1000; }

  return {threads, all, byT, mn, mx};
}

const D = processTrace(TRACE);

/* ── DOM refs ── */
const cv      = document.getElementById('cv');
const ctx     = cv.getContext('2d');
const tip     = document.getElementById('tip');
const det     = document.getElementById('det');
const detH    = document.getElementById('detH');
const detT    = document.getElementById('detT');
const statEl  = document.getElementById('status');

/* ── View state ── */
let vs  = D.mn;   // viewStart: leftmost visible time (µs)
let vsc = 1;      // viewScale: pixels per µs
let sy  = 0;      // vertical scroll offset (px)
let sel = null;   // currently selected event

/* ── Coordinate helpers ── */
const t2x = t  => (t  - vs) * vsc + LW;
const x2t = cx => (cx - LW) / vsc + vs;
const vd  = () => (cv.width - LW) / vsc;  // visible duration in µs

function fitAll() {
  const span = D.mx - D.mn || 1000;
  const mg   = span * 0.02 || 10;
  vs  = D.mn - mg;
  vsc = Math.max(1e-12, (cv.width - LW) / (span + mg * 2));
  sy  = 0;
  render();
}

function zoomAt(f, px) {
  px  = Math.max(LW + 1, px != null ? px : (LW + cv.width) / 2);
  const t = x2t(px);
  vsc = Math.max(1e-12, Math.min(1e12, vsc * f));
  vs  = t - (px - LW) / vsc;
  render();
}

/* ── Time formatting ── */
function ft(us) {
  if (Math.abs(us) >= 1e6) return (us / 1e6).toFixed(3) + ' s';
  if (Math.abs(us) >= 1e3) return (us / 1e3).toFixed(3) + ' ms';
  return us.toFixed(1) + ' \u00b5s';
}
function fd(us) {
  if (us >= 1e6) return (us / 1e6).toFixed(3) + ' s';
  if (us >= 1e3) return (us / 1e3).toFixed(3) + ' ms';
  return us.toFixed(1) + ' \u00b5s';
}

/* ── Ruler tick computation ── */
function niceStep(r) {
  if (r <= 0) return 1;
  const m = Math.pow(10, Math.floor(Math.log10(r)));
  const f = r / m;
  return m * (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10);
}

/* ── Colour helpers ── */
function ecol(e) {
  return (e.cname && CN[e.cname]) ? CN[e.cname]
       : e.name === 'running' ? C_RUN : C_DEF;
}
function contrast(h) {
  const r = parseInt(h.slice(1, 3), 16);
  const g = parseInt(h.slice(3, 5), 16);
  const b = parseInt(h.slice(5, 7), 16);
  return (0.299 * r + 0.587 * g + 0.114 * b) / 255 > 0.55 ? '#111' : '#fff';
}

/* ── Hit test ── */
function hitTest(mx, my) {
  if (mx < LW) return null;
  const ay = my - RH + sy;
  if (ay < 0) return null;
  const li = Math.floor(ay / LH);
  if (li < 0 || li >= D.threads.length) return null;
  const evs = D.byT[D.threads[li].k];
  if (!evs) return null;
  const tc = x2t(mx);
  // Return latest-starting event that covers the cursor (topmost bar).
  let best = null;
  for (const e of evs) {
    if (e.ts <= tc && e._e >= tc)
      if (!best || e.ts > best.ts) best = e;
  }
  return best;
}

/* ── Detail panel ── */
function showDet(e) {
  if (!e) { det.style.display = 'none'; return; }
  detH.textContent = e.name || '(event)';
  const rows = [
    ['ts',  ft(e.ts)],
    ['dur', fd(e.dur || 0)],
    ['tid', e.tid],
    ['pid', e.pid],
  ];
  if (e.cname) rows.push(['cname', e.cname]);
  for (const [k, v] of Object.entries(e.args || {})) rows.push([k, v]);
  detT.innerHTML = rows
    .map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`)
    .join('');
  det.style.display = 'block';
  resize();
}

document.getElementById('detX').onclick = () => {
  sel = null;
  det.style.display = 'none';
  resize();
  render();
};

/* ── Resize ── */
function resize() {
  const w = document.getElementById('wrap');
  cv.width  = Math.max(w.clientWidth  || 0, 100);
  cv.height = Math.max(w.clientHeight || 0, 100);
  render();
}
window.addEventListener('resize', resize);

/* ── Render ── */
function render() {
  const W = cv.width, H = cv.height;
  ctx.clearRect(0, 0, W, H);

  if (D.all.length === 0) {
    ctx.fillStyle = '#888';
    ctx.font = '15px monospace';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('No trace events found in this file.', W / 2, H / 2);
    return;
  }

  /* Clamp vertical scroll */
  const totalH = D.threads.length * LH;
  const maxSY  = Math.max(0, totalH - (H - RH));
  if (sy < 0) sy = 0;
  if (sy > maxSY) sy = maxSY;

  const te = x2t(W);  // rightmost visible time

  /* ── Clip to the area below the ruler ── */
  ctx.save();
  ctx.beginPath(); ctx.rect(0, RH, W, H - RH); ctx.clip();

  /* Lane backgrounds */
  for (let i = 0; i < D.threads.length; i++) {
    const y = RH + i * LH - sy;
    ctx.fillStyle = i % 2 ? '#f5f5f5' : '#fff';
    ctx.fillRect(0, y, W, LH);
    ctx.fillStyle = '#e0e0e0';
    ctx.fillRect(0, y + LH - 1, W, 1);
  }

  /* Vertical grid lines (aligned with ruler ticks) */
  const tgt  = Math.max(4, Math.floor((W - LW) / 80));
  const step = niceStep(vd() / tgt);
  const t0   = Math.ceil(vs / step) * step;
  ctx.fillStyle = '#ebebeb';
  for (let t = t0; t < te + step; t += step) {
    const x = t2x(t);
    if (x >= LW && x <= W) ctx.fillRect(x, RH, 1, H - RH);
  }

  /* Event bars */
  for (let i = 0; i < D.threads.length; i++) {
    const evs = D.byT[D.threads[i].k];
    if (!evs) continue;
    const ly = RH + i * LH - sy;
    for (const e of evs) {
      if (e._e < vs || e.ts > te) continue;
      const x1 = Math.max(LW, t2x(e.ts));
      const x2 = Math.min(W,  t2x(e._e));
      const pw = x2 - x1;
      if (pw < 1) continue;

      const by  = ly + LP;
      const col = ecol(e);
      ctx.fillStyle = col;
      ctx.fillRect(x1, by, pw, BH);

      if (e === sel) {
        ctx.strokeStyle = '#000';
        ctx.lineWidth = 2;
        ctx.strokeRect(x1 + 1, by + 1, pw - 2, Math.max(1, BH - 2));
      }

      /* Label text (clipped to bar) */
      if (pw > 18 && BH > 8) {
        ctx.save();
        ctx.beginPath(); ctx.rect(x1 + 2, by, pw - 4, BH); ctx.clip();
        ctx.fillStyle = contrast(col);
        ctx.font = '11px monospace';
        ctx.textBaseline = 'middle';
        ctx.fillText(e.name || '', x1 + 4, by + BH / 2);
        ctx.restore();
      }
    }
  }

  /* Label column (drawn over lane backgrounds + bars) */
  ctx.fillStyle = '#ebebeb';
  ctx.fillRect(0, RH, LW, H - RH);
  for (let i = 0; i < D.threads.length; i++) {
    const y = RH + i * LH - sy;
    if (i % 2) { ctx.fillStyle = '#e0e0e0'; ctx.fillRect(0, y, LW, LH); }
    ctx.fillStyle = '#ccc';
    ctx.fillRect(0, y + LH - 1, LW, 1);
    ctx.save();
    ctx.beginPath(); ctx.rect(8, y + 1, LW - 12, LH - 2); ctx.clip();
    ctx.fillStyle = '#333';
    ctx.font = '11px monospace';
    ctx.textBaseline = 'middle';
    ctx.fillText(D.threads[i].name, 10, y + LH / 2);
    ctx.restore();
  }
  /* Label column right border */
  ctx.fillStyle = '#bbb'; ctx.fillRect(LW - 1, RH, 1, H - RH);

  ctx.restore();  /* end below-ruler clip */

  /* ── Ruler ── */
  ctx.fillStyle = '#f0f0f0'; ctx.fillRect(0, 0, W, RH);
  ctx.fillStyle = '#ccc';    ctx.fillRect(0, RH - 1, W, 1);

  ctx.font = '10px monospace';
  ctx.textBaseline = 'top';
  for (let t = t0; t < te + step; t += step) {
    const x = t2x(t);
    if (x < LW || x > W) continue;
    /* tick mark */
    ctx.fillStyle = '#bbb';
    ctx.fillRect(x, Math.round(RH * 0.55), 1, Math.round(RH * 0.45));
    /* label */
    ctx.fillStyle = '#555';
    ctx.fillText(ft(t - D.mn), x + 2, 5);
  }

  /* Top-left corner */
  ctx.fillStyle = '#e0e0e0'; ctx.fillRect(0, 0, LW, RH);
  ctx.fillStyle = '#ccc';
  ctx.fillRect(LW - 1, 0, 1, RH);
  ctx.fillRect(0, RH - 1, LW, 1);
  ctx.fillStyle = '#555';
  ctx.font = '11px monospace';
  ctx.textBaseline = 'middle';
  ctx.fillText('Thread', 10, RH / 2);

  statEl.textContent =
    `visible: ${fd(vd())}  |  ${D.all.length} events  |  ${D.threads.length} threads`;
}

/* ── Mouse / wheel interaction ── */
let pan = null;  // {x0, y0, vs0, sy0} while dragging

cv.addEventListener('wheel', e => {
  e.preventDefault();
  const f = e.deltaY < 0 ? 1.25 : 1 / 1.25;
  if (e.offsetX > LW) {
    zoomAt(f, e.offsetX);
  } else {
    /* vertical scroll when pointer is over the label column */
    sy = Math.max(0, sy + e.deltaY * 0.6);
    render();
  }
}, {passive: false});

cv.addEventListener('mousedown', e => {
  pan = {x0: e.offsetX, y0: e.offsetY, vs0: vs, sy0: sy};
  cv.style.cursor = 'grabbing';
});

cv.addEventListener('mousemove', e => {
  if (pan) {
    vs = pan.vs0 - (e.offsetX - pan.x0) / vsc;
    sy = pan.sy0 - (e.offsetY - pan.y0);
    render();
    tip.style.display = 'none';
    return;
  }
  /* Hover tooltip */
  const ev = hitTest(e.offsetX, e.offsetY);
  if (ev) {
    const lines = [ev.name || '(event)', `ts:  ${ft(ev.ts)}`, `dur: ${fd(ev.dur || 0)}`];
    for (const [k, v] of Object.entries(ev.args || {}).slice(0, 5))
      lines.push(`${k}: ${v}`);
    tip.textContent = lines.join('\n');
    let tx = e.offsetX + 14, ty = e.offsetY + 14;
    if (tx + 280 > cv.width)  tx = e.offsetX - 290;
    if (ty + 110 > cv.height) ty = e.offsetY - 120;
    tip.style.left    = tx + 'px';
    tip.style.top     = ty + 'px';
    tip.style.display = 'block';
  } else {
    tip.style.display = 'none';
  }
});

cv.addEventListener('mouseup', e => {
  if (!pan) return;
  const dx = Math.abs(e.offsetX - pan.x0);
  const dy = Math.abs(e.offsetY - pan.y0);
  cv.style.cursor = 'crosshair';
  if (dx < 4 && dy < 4) {
    /* treat as click — select event and show detail */
    sel = hitTest(e.offsetX, e.offsetY);
    showDet(sel);
  }
  pan = null;
  render();
});

cv.addEventListener('mouseleave', () => {
  tip.style.display = 'none';
  if (pan) { pan = null; cv.style.cursor = 'crosshair'; }
});

/* ── Keyboard shortcuts ── */
document.addEventListener('keydown', e => {
  if (e.target !== document.body) return;
  const panStep = (cv.width - LW) * 0.15;
  switch (e.key) {
    case '+': case '=': zoomAt(1.4); break;
    case '-': case '_': zoomAt(1 / 1.4); break;
    case 'f': case 'F': fitAll(); break;
    case 'ArrowLeft':  vs -= panStep / vsc; render(); break;
    case 'ArrowRight': vs += panStep / vsc; render(); break;
    case 'ArrowUp':    sy = Math.max(0, sy - LH * 3); render(); break;
    case 'ArrowDown':  sy += LH * 3; render(); break;
    case 'Escape':
      sel = null; det.style.display = 'none'; resize(); render(); break;
  }
});

/* ── Toolbar buttons ── */
document.getElementById('bFit').onclick = fitAll;
document.getElementById('bZI').onclick  = () => zoomAt(1.4);
document.getElementById('bZO').onclick  = () => zoomAt(1 / 1.4);

/* ── Init ── */
window.addEventListener('load', () => { resize(); fitAll(); });
</script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("input", help="trace.json input file")
    ap.add_argument(
        "-o", "--output", default=None,
        help="output HTML path (default: replaces .json extension with .html)",
    )
    ap.add_argument(
        "--open", action="store_true",
        help="open the generated HTML in the system default browser after writing",
    )
    args = ap.parse_args()

    with open(args.input, encoding="utf-8") as f:
        trace = json.load(f)

    out = args.output or (os.path.splitext(args.input)[0] + ".html")

    # Escape any </script> sequences inside the embedded JSON so the HTML
    # parser does not mistake them for the end of the <script> block.
    trace_json = json.dumps(trace).replace("</", "<\\/")

    filename = os.path.basename(args.input)
    html = _TEMPLATE.replace("__TRACE_DATA__", trace_json)
    html = html.replace("__FILENAME__", filename)

    with open(out, "w", encoding="utf-8") as f:
        f.write(html)

    print(f"Written: {out}", file=sys.stderr)

    if args.open:
        url = "file://" + os.path.abspath(out)
        print(f"Opening: {url}", file=sys.stderr)
        webbrowser.open(url)


if __name__ == "__main__":
    main()
