// Small canvas chart engine for the tank pages (no libraries: the board may have no internet).
// Chart(box, opt): box = <div class="chartbox"> holding a <canvas>; opt is replaced with chart.set(newOpt).
//   xmode 'time' (x = epoch seconds, x0/x1 = domain) or 'cat' (x = category index, cats = labels)
//   left / right: {min,max,fmt,unit,title}  (min/max optional: auto "nice" scale, starting at 0)
//   series: [{id,name,color,type:'area'|'line'|'step'|'bar'|'runbar', axis:'left'|'right', pts:[[x,y,extra]], fmt(y), visible}]
//   bands: [[x0,x1]] shaded spans (pump runs); hlines: [{y,axis,label}] dashed reference lines
//   tip(info) -> html: info = {x, label, items:[{s,y,p}]}   empty: text shown when nothing is plotted
(function () {
  const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  function niceScale(min, max, n) {
    if (!(max > min)) max = min + 1;
    const raw = (max - min) / Math.max(1, n), p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
    const step = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * p;
    return { min: Math.floor(min / step) * step, max: Math.ceil(max / step) * step, step };
  }
  const TSTEPS = [300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 172800, 604800];
  function timeTicks(t0, t1, max) {
    let step = TSTEPS.find(s => (t1 - t0) / s <= max) || TSTEPS[TSTEPS.length - 1];
    const off = -new Date(t0 * 1000).getTimezoneOffset() * 60, out = [];
    for (let t = Math.ceil((t0 + off) / step) * step - off; t <= t1; t += step) out.push(t);
    return { step, ticks: out };
  }
  const p2 = n => String(n).padStart(2, '0');
  function tlabel(t, step) {
    const d = new Date(t * 1000);
    if (step >= 86400 || (d.getHours() === 0 && d.getMinutes() === 0)) return d.toLocaleDateString([], { month: 'short', day: 'numeric' });
    return p2(d.getHours()) + ':' + p2(d.getMinutes());
  }

  class Chart {
    constructor(box, opt) {
      this.box = box; this.cv = box.querySelector('canvas');
      this.tip = document.createElement('div'); this.tip.className = 'tip'; this.tip.hidden = true; box.appendChild(this.tip);
      this.opt = opt || {}; this.hover = null;
      const mv = e => { const r = this.cv.getBoundingClientRect(); this.hover = { x: e.clientX - r.left, y: e.clientY - r.top }; this.draw() };
      this.cv.addEventListener('pointermove', mv);
      this.cv.addEventListener('pointerdown', mv);
      this.cv.addEventListener('pointerleave', e => { if (e.pointerType === 'mouse') { this.hover = null; this.draw() } });
      this.cv.addEventListener('keydown', e => { if (e.key === 'Escape') { this.hover = null; this.draw() } });
      new ResizeObserver(() => this.draw()).observe(box);
    }
    set(opt) { this.opt = opt; this.draw() }
    vis() { return (this.opt.series || []).filter(s => s.visible !== false && s.pts && s.pts.length) }

    draw() {
      const o = this.opt, cv = this.cv, W = cv.clientWidth, H = cv.clientHeight; if (!W || !H) return;
      const dpr = window.devicePixelRatio || 1;
      if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr) }
      const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
      const mute = css('--mute'), line = css('--line'), fg = css('--fg'), small = W < 480;
      g.font = (small ? 11 : 12) + 'px system-ui,sans-serif';
      const S = this.vis(), cat = o.xmode === 'cat';
      if (!S.length) {                                           // nothing to plot: just the message (no misleading 0..1 axis)
        g.strokeStyle = line; g.beginPath(); g.moveTo(10, H - 24.5); g.lineTo(W - 10, H - 24.5); g.stroke();
        g.fillStyle = mute; g.textAlign = 'center'; g.textBaseline = 'middle'; g.fillText(o.empty || 'No data yet', W / 2, H / 2); this.tipHide(); return
      }
      // ---- scales ----
      const ax = {};
      for (const side of ['left', 'right']) {
        const a = o[side]; if (!a) continue;
        let lo = a.min, hi = a.max;
        const mine = S.filter(s => (s.axis || 'left') === side);
        if (hi === undefined) { hi = 0; for (const s of mine) for (const p of s.pts) if (p[1] > hi) hi = p[1]; for (const h of (o.hlines || [])) if ((h.axis || 'left') === side && h.y > hi) hi = h.y; hi = hi * 1.08 || 1 }
        if (lo === undefined) lo = 0;
        const sc = a.max !== undefined && a.min !== undefined ? { min: lo, max: hi, step: a.step || (hi - lo) / 4 } : niceScale(lo, hi, small ? 4 : 5);
        ax[side] = Object.assign(sc, { fmt: a.fmt || (v => String(+v.toFixed(2))) });
      }
      g.textBaseline = 'middle';
      const lw = side => { const a = ax[side]; if (!a) return 0; let w = 0; for (let v = a.min; v <= a.max + 1e-9; v += a.step) w = Math.max(w, g.measureText(a.fmt(v)).width); return w + 10 };
      const L = Math.max(34, lw('left')), R = ax.right ? lw('right') : 8, T = 10, B = 24, pw = W - L - R, ph = H - T - B;
      const Y = (side, v) => T + ph * (1 - (v - ax[side].min) / (ax[side].max - ax[side].min || 1));
      let X, x0 = o.x0, x1 = o.x1, nc = 0;
      if (cat) { nc = (o.cats || []).length; X = i => L + pw * (i + .5) / (nc || 1) }
      else { if (x0 === undefined) { x0 = Infinity; x1 = -Infinity; for (const s of S) for (const p of s.pts) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]) } } X = t => L + pw * (t - x0) / ((x1 - x0) || 1) }
      this.geom = { L, R, T, B, pw, ph, X, Y, x0, x1, W, H, cat, nc };
      // ---- grid + axes ----
      g.lineWidth = 1; g.strokeStyle = line; g.fillStyle = mute;
      const primary = ax.left ? 'left' : 'right';
      for (let v = ax[primary].min; v <= ax[primary].max + 1e-9; v += ax[primary].step) {
        const y = Math.round(Y(primary, v)) + .5; g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke();
      }
      for (const side of ['left', 'right']) if (ax[side]) {
        g.textAlign = side === 'left' ? 'right' : 'left'; g.fillStyle = side === 'left' || !ax.left ? mute : css('--acc');
        for (let v = ax[side].min; v <= ax[side].max + 1e-9; v += ax[side].step) g.fillText(ax[side].fmt(v), side === 'left' ? L - 6 : W - R + 6, Y(side, v));
      }
      g.fillStyle = mute; g.textBaseline = 'alphabetic'; g.textAlign = 'center';
      if (cat) {
        const every = Math.max(1, Math.ceil(nc / Math.max(2, Math.floor(pw / (small ? 38 : 46)))));
        (o.cats || []).forEach((c, i) => { if (i % every === 0 || i === nc - 1) g.fillText(c, X(i), H - 7) });
      } else if (isFinite(x0) && x1 > x0) {
        const tt = timeTicks(x0, x1, Math.max(2, Math.floor(pw / (small ? 64 : 80))));
        for (const t of tt.ticks) { const x = Math.round(X(t)) + .5; g.strokeStyle = line; g.beginPath(); g.moveTo(x, T + ph); g.lineTo(x, T + ph + 4); g.stroke(); if (x > L + 14 && x < W - R - 14) g.fillText(tlabel(t, tt.step), x, H - 7) }
      }
      if (!S.length) { g.fillStyle = mute; g.textAlign = 'center'; g.fillText(o.empty || 'No data yet', L + pw / 2, T + ph / 2); this.tipHide(); return }
      // ---- bands (pump runs) ----
      g.save(); g.beginPath(); g.rect(L, T, pw, ph); g.clip();
      for (const b of (o.bands || [])) { g.fillStyle = css('--ok') + '26'; const xa = X(b[0]), xb = X(b[1]); g.fillRect(xa, T, Math.max(2, xb - xa), ph) }
      // ---- reference lines ----
      g.setLineDash([5, 4]); g.lineWidth = 1;
      for (const h of (o.hlines || [])) {
        const side = h.axis || 'left'; if (!ax[side]) continue; const y = Y(side, h.y); g.strokeStyle = h.color || mute; g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke();
        if (h.label) { g.setLineDash([]); g.textAlign = 'right'; const tw = g.measureText(h.label).width; g.save(); g.globalAlpha = .88; g.fillStyle = css('--card'); g.fillRect(W - R - tw - 10, y - 17, tw + 8, 15); g.restore(); g.fillStyle = h.color || mute; g.fillText(h.label, W - R - 6, y - 5); g.setLineDash([5, 4]) }
      }
      g.setLineDash([]);
      // ---- series ----
      const nser = S.filter(s => s.type === 'bar').length; let bi = 0;
      for (const s of S) {
        const side = s.axis || 'left'; if (!ax[side]) continue; const col = s.color || css('--water'), y0 = Y(side, Math.max(0, ax[side].min));
        if (s.type === 'bar') {
          const slot = pw / (nc || 1), bw = Math.max(2, Math.min(34, slot * .8 / nser)), off = (bi - (nser - 1) / 2) * bw; bi++;
          for (const p of s.pts) { const cx = X(p[0]) + off, top = Y(side, p[1]); g.fillStyle = col; if (p[2] && p[2].partial) { g.globalAlpha = .55 } g.fillRect(cx - bw / 2, top, bw - 1, y0 - top); g.globalAlpha = 1 }
          continue;
        }
        if (s.type === 'runbar') {
          for (const p of s.pts) { const xa = X(p[0]), xb = X(p[2].end), top = Y(side, p[1]); g.fillStyle = col + 'cc'; g.fillRect(xa, top, Math.max(3, xb - xa), y0 - top); g.strokeStyle = col; g.strokeRect(xa + .5, top + .5, Math.max(3, xb - xa) - 1, y0 - top - 1) }
          continue;
        }
        const pts = this.reduce(s.pts, pw, s.type === 'step'); if (!pts.length) continue;
        g.beginPath();
        pts.forEach((p, i) => { const x = X(p[0]), y = Y(side, p[1]); if (!i) g.moveTo(x, y); else if (s.type === 'step') { g.lineTo(x, Y(side, pts[i - 1][1])); g.lineTo(x, y) } else g.lineTo(x, y) });
        if (s.type === 'area' || s.type === 'step') {
          const last = pts[pts.length - 1], first = pts[0]; g.save(); g.lineTo(X(last[0]), y0); g.lineTo(X(first[0]), y0); g.closePath();
          const gr = g.createLinearGradient(0, T, 0, T + ph); gr.addColorStop(0, col + '66'); gr.addColorStop(1, col + '0d'); g.fillStyle = gr; g.fill(); g.restore();
          g.beginPath(); pts.forEach((p, i) => { const x = X(p[0]), y = Y(side, p[1]); if (!i) g.moveTo(x, y); else if (s.type === 'step') { g.lineTo(x, Y(side, pts[i - 1][1])); g.lineTo(x, y) } else g.lineTo(x, y) });
        }
        g.strokeStyle = col; g.lineWidth = 2; g.lineJoin = 'round'; g.stroke(); g.lineWidth = 1;
      }
      g.restore();
      if (this.hover) this.drawHover(g, S, ax, fg); else this.tipHide();
    }

    // one point per ~pixel: average the points that fall in the same pixel column (keeps long ranges fast and legible)
    reduce(pts, pw, keepLast) {
      const n = pts.length; if (n <= pw * 1.5) return pts;
      const { x0, x1 } = this.geom, out = [], k = pw; let bx = -1, sx = 0, sy = 0, c = 0;
      const flush = () => { if (c) out.push([sx / c, sy / c]) };
      for (const p of pts) { if (p[0] < x0 || p[0] > x1) continue; const b = Math.floor((p[0] - x0) / ((x1 - x0) || 1) * k); if (b !== bx) { flush(); bx = b; sx = sy = c = 0 } sx += p[0]; sy += p[1]; c++ }
      flush(); return out;
    }

    drawHover(g, S, ax, fg) {
      const { L, T, pw, ph, X, x0, x1, cat, nc, W } = this.geom, hx = clamp(this.hover.x, L, L + pw);
      let key, label; const items = [];
      if (cat) { key = clamp(Math.round((hx - L) / pw * nc - .5), 0, nc - 1); label = (this.opt.catLabel || (i => this.opt.cats[i]))(key) }
      else { key = x0 + (hx - L) / pw * (x1 - x0); label = null }
      for (const s of S) {
        let best = null, bd = Infinity;
        for (const p of s.pts) {
          let d;
          if (cat) d = Math.abs(p[0] - key);
          else if (s.type === 'runbar') d = key >= p[0] && key <= p[2].end ? 0 : Math.min(Math.abs(p[0] - key), Math.abs(p[2].end - key)) + 1;
          else d = Math.abs(p[0] - key);
          if (d < bd) { bd = d; best = p }
        }
        if (!best) continue;
        const tol = cat ? .5 : (x1 - x0) * 26 / pw; if (bd > tol && s.type !== 'runbar') continue; if (s.type === 'runbar' && bd > tol) continue;
        items.push({ s, y: best[1], p: best });
      }
      if (!items.length && !cat) { this.tipHide(); return }
      const t = cat ? key : items[0].p[0];
      const gx = cat ? X(key) : X(t);
      g.save(); g.strokeStyle = fg; g.globalAlpha = .35; g.setLineDash([3, 3]); g.beginPath(); g.moveTo(Math.round(gx) + .5, T); g.lineTo(Math.round(gx) + .5, T + ph); g.stroke(); g.restore();
      for (const it of items) if (it.s.type !== 'bar' && it.s.type !== 'runbar') { const side = it.s.axis || 'left'; g.beginPath(); g.arc(X(it.p[0]), this.geom.Y(side, it.y), 4, 0, 7); g.fillStyle = css('--card'); g.fill(); g.lineWidth = 2; g.strokeStyle = it.s.color || css('--water'); g.stroke(); g.lineWidth = 1 }
      const html = this.opt.tip ? this.opt.tip({ x: t, label, items, key }) : '';
      if (!html) { this.tipHide(); return }
      const tip = this.tip; tip.innerHTML = html; tip.hidden = false;
      const tw = tip.offsetWidth, th = tip.offsetHeight;
      let tx = gx + 14; if (tx + tw > W - 4) tx = gx - tw - 14; tx = Math.max(4, tx);
      tip.style.transform = 'translate(' + Math.round(tx) + 'px,' + Math.round(clamp(this.hover.y - th / 2, 4, this.geom.H - th - 4)) + 'px)';
    }
    tipHide() { if (this.tip) this.tip.hidden = true }
  }
  window.Chart = Chart;
  window.ChartUtil = { p2 };
})();
