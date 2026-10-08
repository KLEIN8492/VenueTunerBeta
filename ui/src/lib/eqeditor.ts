// Interactive filter editor for one output: magnitude + phase graphs with draggable
// handles, PEQ / crossover / all-pass / FIR panels and "predicted after EQ".
//
// Predicted = captured trace + (filters now − filters at capture). Captured traces carry
// the outputs' processing from when they were taken (see measurement.ts).
import { Plot, type PlotTrace } from './plot';
import { outputResponse, peqSection, response, magDb, wrapDeg } from './filters';
import { coherenceAlpha } from './coherence';
import { esc } from './statusbar';
import { samplesToMs } from './scale';
import type { Engine, TfFrame } from './engine';
import type { ApBand, OutputConfig, PeqBand, PeqType, Trace, XoType } from './types';

const FREQS = Array.from({ length: 360 }, (_, i) => 20 * Math.pow(1000, i / 359));
const PEQ_TYPES: PeqType[] = ['bell', 'lowshelf', 'highshelf', 'notch', 'lowpass', 'highpass'];
const XO_TYPES: XoType[] = ['off', 'bw6', 'bw12', 'bw18', 'bw24', 'bw36', 'bw48', 'lr12', 'lr24', 'lr48'];
const MAX_PEQ = 16, MAX_AP = 4;
const css = (n: string) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

type Handle = { kind: 'peq' | 'hp' | 'lp' | 'ap'; index: number };

// Mean level 200 Hz–5 kHz. Measured traces are shifted by (their mean − the filter
// curve's mean) so they overlay the curve wherever the system follows the filters.
function normOffset(freq: ArrayLike<number>, mag: ArrayLike<number>): number {
  let s = 0, n = 0;
  for (let i = 0; i < freq.length; i++) if (freq[i] >= 200 && freq[i] <= 5000 && isFinite(mag[i])) { s += mag[i]; n++; }
  return n ? s / n : 0;
}

const loadTraces = (): Trace[] => { try { return JSON.parse(localStorage.getItem('vt.traces') ?? '[]'); } catch { return []; } };
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));
const round = (v: number, step: number) => Math.round(v / step) * step;

export class EqEditor {
  private out = -1;
  private cfg: OutputConfig | null = null;
  private drag: Handle | null = null;
  private selected: Handle | null = null;
  private live: TfFrame | null = null;
  private baseName = '';
  private showLive = true;
  private linked = false;
  private pending = new Map<string, unknown>();
  private flushTimer = 0;
  private inFlight = 0; // patches sent, reply not yet received
  private mag: Plot;
  private phase: Plot;
  private side: HTMLElement;

  constructor(root: HTMLElement, private engine: Engine, private selection: () => number[]) {
    root.innerHTML = `
      <div class="eq">
        <div class="eq-graphs">
          <div class="cv"><canvas class="eq-mag"></canvas></div>
          <div class="cv"><canvas class="eq-ph"></canvas></div>
        </div>
        <div class="eq-side"></div>
      </div>`;
    this.side = root.querySelector('.eq-side')!;
    this.mag = new Plot(root.querySelector('.eq-mag')!, { xMin: 20, xMax: 20000, logX: true, yMin: -24, yMax: 12, yStep: 6, yUnit: 'dB', title: 'FILTERS', yLocked: true });
    this.phase = new Plot(root.querySelector('.eq-ph')!, { xMin: 20, xMax: 20000, logX: true, yMin: -180, yMax: 180, yStep: 90, yUnit: 'deg', title: 'PHASE', yLocked: true });
    this.mag.onOverlay = (ctx) => this.drawHandles(ctx);
    this.bindPointer(root.querySelector('.eq-mag')!);
    this.side.addEventListener('change', (e) => this.onPanelChange(e));
    this.side.addEventListener('click', (e) => this.onPanelClick(e));
    this.side.addEventListener('focusout', () => setTimeout(() => this.renderPanel(), 0));
    engine.on('state', () => this.adoptEngineState());
    engine.on('tf', (f) => { this.live = f; if (this.showLive) this.redraw(); });
    window.addEventListener('storage', (e) => { if (e.key === 'vt.traces') this.render(); });
  }

  setOutput(i: number): void {
    this.out = i;
    this.selected = null;
    const o = this.engine.state?.outputs[i];
    this.cfg = o ? structuredClone(o) : null;
    this.render();
  }

  // Take the engine's copy of this output, unless local edits are still on their way:
  // an older state arriving mid-edit would otherwise resurrect a deleted band.
  private adoptEngineState(): void {
    const o = this.engine.state?.outputs[this.out];
    if (this.out < 0 || !o || this.drag || this.pending.size || this.inFlight) return;
    this.cfg = structuredClone(o);
    this.render();
  }

  private sr(): number { return this.engine.status?.sampleRate || this.engine.state?.device.sampleRate || 48000; }

  // ---------------------------------------------------------------- engine patches
  // Coalesces rapid edits (dragging) to at most one engine round-trip every 40 ms. With
  // "link" on, the same edit is applied to every selected output.
  private send(field: string, value: unknown): void {
    this.pending.set(field, value);
    if (this.flushTimer) return;
    this.flushTimer = window.setTimeout(() => {
      this.flushTimer = 0;
      const targets = this.linked ? [...new Set([this.out, ...this.selection()])] : [this.out];
      const patches = [...this.pending].flatMap(([f, v]) => targets.map((o) => ({ path: `outputs/${o}/${f}`, value: v })));
      this.pending.clear();
      this.inFlight++;
      this.engine.patchMany(patches)
        .catch((e: Error) => alert(e.message))
        .finally(() => { this.inFlight--; this.adoptEngineState(); });
    }, 40);
  }
  private sendPeq(): void { this.send('peq', this.cfg!.peq); }
  private sendAp(): void { this.send('ap', this.cfg!.ap); }

  // ---------------------------------------------------------------- drawing
  private render(): void {
    this.renderPanel();
    this.redraw();
  }

  private redraw(): void {
    const o = this.cfg;
    if (!o) { this.mag.traces = []; this.phase.traces = []; this.mag.draw(); this.phase.draw(); return; }
    const sr = this.sr();
    const now = outputResponse(o, sr, FREQS);
    const magT: PlotTrace[] = [], phT: PlotTrace[] = [];

    const base = loadTraces().find((t) => t.name === this.baseName);
    if (base) {
      const atCapture = base.outputs?.[this.out];
      const cNow = outputResponse(o, sr, base.freq);
      const cThen = atCapture ? outputResponse(atCapture, sr, base.freq) : cNow;
      const off = normOffset(base.freq, base.mag) - normOffset(base.freq, cThen.magDb);
      magT.push({ x: base.freq, y: base.mag.map((m) => m - off), color: '#6b7075', label: 'BASE' });
      magT.push({ x: base.freq, y: base.mag.map((m, i) => m - off + cNow.magDb[i] - cThen.magDb[i]), color: '#ffd166', width: 1.75, label: 'PRED' });
      phT.push({ x: base.freq, y: base.phase, color: '#6b7075', wrap: 180 });
      phT.push({ x: base.freq, y: base.phase.map((p, i) => wrapDeg(p + cNow.phaseDeg[i] - cThen.phaseDeg[i])), color: '#ffd166', wrap: 180 });
    }
    if (this.showLive && this.live && this.live.frames > 0) {
      const off = normOffset(this.live.freq, this.live.mag) - normOffset(this.live.freq, outputResponse(o, sr, this.live.freq).magDb);
      const alpha = Array.from(this.live.coh, coherenceAlpha);
      magT.push({ x: this.live.freq, y: Array.from(this.live.mag, (m) => m - off), color: css('--accent'), alpha, label: 'LIVE' });
      phT.push({ x: this.live.freq, y: this.live.phase, color: css('--accent'), alpha, wrap: 180 });
    }
    if (this.selected?.kind === 'peq' && o.peq[this.selected.index]) {
      const s = peqSection({ ...o.peq[this.selected.index], on: true }, sr);
      magT.push({ x: FREQS, y: FREQS.map((f) => magDb(response(s, f, sr))), color: '#ffffff55' });
    }
    magT.push({ x: FREQS, y: now.magDb, color: '#ffffff', width: 2, label: 'EQ' });
    phT.push({ x: FREQS, y: now.phaseDeg, color: '#ffffff', width: 1.5, wrap: 180 });
    this.mag.traces = magT;
    this.phase.traces = phT;
    this.mag.draw();
    this.phase.draw();
  }

  private handles(): { h: Handle; f: number; y: number; label: string }[] {
    const o = this.cfg;
    if (!o) return [];
    const top = this.mag.opts.yMax - 2;
    const out: { h: Handle; f: number; y: number; label: string }[] = [];
    o.peq.forEach((b, i) => out.push({ h: { kind: 'peq', index: i }, f: b.freq, y: ['bell', 'lowshelf', 'highshelf'].includes(b.type) ? b.gainDb : 0, label: String(i + 1) }));
    if (o.hp.type !== 'off') out.push({ h: { kind: 'hp', index: 0 }, f: o.hp.freq, y: -6, label: 'HP' });
    if (o.lp.type !== 'off') out.push({ h: { kind: 'lp', index: 0 }, f: o.lp.freq, y: -6, label: 'LP' });
    o.ap.forEach((a, i) => out.push({ h: { kind: 'ap', index: i }, f: a.freq, y: top, label: `A${i + 1}` }));
    return out;
  }

  private drawHandles(ctx: CanvasRenderingContext2D): void {
    const sel = this.selected;
    ctx.font = `9px ${css('--mono')}`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (const { h, f, y, label } of this.handles()) {
      const [px, py] = this.mag.toPx(f, y);
      const isSel = sel && sel.kind === h.kind && sel.index === h.index;
      const off = (h.kind === 'peq' && !this.cfg!.peq[h.index].on) || (h.kind === 'ap' && !this.cfg!.ap[h.index].on);
      ctx.fillStyle = isSel ? '#ffd166' : off ? '#3a3d41' : '#2a2d31';
      ctx.strokeStyle = isSel ? '#ffd166' : '#d6d8da';
      ctx.beginPath();
      if (h.kind === 'peq') ctx.arc(px, py, 8, 0, Math.PI * 2);
      else ctx.rect(px - 9, py - 7, 18, 14);
      ctx.fill();
      ctx.stroke();
      ctx.fillStyle = isSel ? '#000' : '#d6d8da';
      ctx.fillText(label, px, py + 0.5);
    }
  }

  // ---------------------------------------------------------------- pointer interaction
  private hit(e: MouseEvent): Handle | null {
    const { padL, padT } = this.mag.geom;
    let best: Handle | null = null, bestD = 11;
    for (const { h, f, y } of this.handles()) {
      const [px, py] = this.mag.toPx(f, y);
      const d = Math.hypot(e.offsetX - padL - px, e.offsetY - padT - py);
      if (d < bestD) { bestD = d; best = h; }
    }
    return best;
  }

  private bindPointer(canvas: HTMLCanvasElement): void {
    canvas.addEventListener('mousedown', (e) => {
      if (!this.cfg) return;
      const h = this.hit(e);
      if (h && e.altKey) { this.removeHandle(h); return; }
      this.selected = h;
      this.drag = h;
      this.render();
    });
    window.addEventListener('mousemove', (e) => {
      if (!this.drag || !this.cfg) return;
      const r = canvas.getBoundingClientRect();
      const [f0, g0] = this.mag.fromOffset(e.clientX - r.left, e.clientY - r.top);
      const f = round(clamp(f0, 10, 24000), f0 < 100 ? 0.5 : f0 < 1000 ? 1 : 10);
      const g = round(clamp(g0, -30, 15), 0.1);
      const o = this.cfg, h = this.drag;
      if (h.kind === 'peq') {
        const b = o.peq[h.index];
        b.freq = f;
        if (['bell', 'lowshelf', 'highshelf'].includes(b.type)) b.gainDb = g;
        this.sendPeq();
      } else if (h.kind === 'hp' || h.kind === 'lp') {
        o[h.kind].freq = f;
        this.send(`${h.kind}/freq`, f);
      } else {
        o.ap[h.index].freq = f;
        this.sendAp();
      }
      this.redraw();
      this.updatePanelValues();
    });
    window.addEventListener('mouseup', () => { if (this.drag) { this.drag = null; this.adoptEngineState(); } });
    canvas.addEventListener('wheel', (e) => {
      if (!this.cfg) return;
      const h = this.hit(e) ?? this.selected;
      if (!h || (h.kind !== 'peq' && h.kind !== 'ap')) return;
      e.preventDefault();
      const k = Math.pow(1.12, -Math.sign(e.deltaY));
      if (h.kind === 'peq') { const b = this.cfg.peq[h.index]; b.q = round(clamp(b.q * k, 0.1, 30), 0.01); this.sendPeq(); }
      else { const a = this.cfg.ap[h.index]; a.q = round(clamp(a.q * k, 0.1, 10), 0.01); this.sendAp(); }
      this.selected = h;
      this.redraw();
      this.updatePanelValues();
    }, { passive: false });
    canvas.addEventListener('dblclick', (e) => {
      if (!this.cfg || this.hit(e) || this.cfg.peq.length >= MAX_PEQ) return;
      const [f, g] = this.mag.fromOffset(e.offsetX, e.offsetY);
      this.cfg.peq.push({ on: true, type: 'bell', freq: round(clamp(f, 10, 24000), 1), gainDb: round(clamp(g, -30, 15), 0.5), q: 1.4 });
      this.selected = { kind: 'peq', index: this.cfg.peq.length - 1 };
      this.sendPeq();
      this.render();
    });
  }

  private removeHandle(h: Handle): void {
    const o = this.cfg!;
    if (h.kind === 'peq') { o.peq.splice(h.index, 1); this.sendPeq(); }
    else if (h.kind === 'ap') { o.ap.splice(h.index, 1); this.sendAp(); }
    else { o[h.kind].type = 'off'; this.send(`${h.kind}/type`, 'off'); }
    this.selected = null;
    this.render();
  }

  // ---------------------------------------------------------------- side panel
  private renderPanel(): void {
    // Don't rebuild under a field the user is typing in; buttons/checkboxes never block.
    const a = document.activeElement as HTMLInputElement | null;
    if (a && this.side.contains(a) && (a.tagName === 'SELECT' || (a.tagName === 'INPUT' && (a.type === 'number' || a.type === 'text')))) return;
    const o = this.cfg;
    if (!o) { this.side.innerHTML = '<div class="dim">Select an output strip to edit its filters.</div>'; return; }
    const sr = this.sr();
    const traces = loadTraces();
    const opt = (v: string, l: string, s: boolean) => `<option value="${esc(v)}"${s ? ' selected' : ''}>${esc(l)}</option>`;
    const num = (field: string, v: number, step: number, min: number, max: number, w = 58) =>
      `<input type="number" data-f="${field}" value="${+v.toFixed(3)}" step="${step}" min="${min}" max="${max}" style="width:${w}px"/>`;
    const isSel = (k: Handle['kind'], i: number) => this.selected?.kind === k && this.selected.index === i ? ' class="sel"' : '';

    this.side.innerHTML = `
      <div class="row"><b class="grow">${this.out + 1} · ${esc(o.name)}</b>
        <label title="apply edits to all selected strips"><input type="checkbox" data-ui="linked" ${this.linked ? 'checked' : ''}/> link</label></div>
      <div class="row"><label>Base</label><select data-ui="base" class="grow">${opt('', '— none —', !this.baseName)}${traces.map((t) => opt(t.name, t.name, t.name === this.baseName)).join('')}</select></div>
      <div class="row"><label><input type="checkbox" data-ui="live" ${this.showLive ? 'checked' : ''}/> live TF</label><span class="dim">traces level-matched to the EQ curve</span></div>

      <h2>PEQ ${o.peq.length}/${MAX_PEQ}</h2>
      <table class="eqt"><tr><th></th><th>#</th><th>type</th><th>Hz</th><th>dB</th><th>Q</th><th></th></tr>
      ${o.peq.map((b: PeqBand, i) => `<tr data-k="peq" data-i="${i}"${isSel('peq', i)}>
        <td><input type="checkbox" data-f="on" ${b.on ? 'checked' : ''}/></td><td>${i + 1}</td>
        <td><select data-f="type">${PEQ_TYPES.map((t) => opt(t, t, t === b.type)).join('')}</select></td>
        <td>${num('freq', b.freq, 1, 10, 24000, 62)}</td><td>${num('gainDb', b.gainDb, 0.1, -30, 15, 50)}</td><td>${num('q', b.q, 0.05, 0.1, 30, 46)}</td>
        <td><button data-act="del">×</button></td></tr>`).join('')}
      </table>
      <div class="row"><button data-act="addPeq" ${o.peq.length >= MAX_PEQ ? 'disabled' : ''}>+ band</button><span class="dim">dbl-click graph: add · drag · wheel: Q · ⌥-click: delete</span></div>

      <h2>Crossover</h2>
      ${(['hp', 'lp'] as const).map((k) => `<div class="row" data-k="${k}"><label>${k.toUpperCase()}</label>
        <select data-f="type">${XO_TYPES.map((t) => opt(t, t.toUpperCase(), t === o[k].type)).join('')}</select>${num('freq', o[k].freq, 1, 10, 24000, 66)}<span class="dim">Hz</span></div>`).join('')}

      <h2>All-pass ${o.ap.length}/${MAX_AP}</h2>
      <table class="eqt"><tr><th></th><th>#</th><th>order</th><th>Hz</th><th>Q</th><th></th></tr>
      ${o.ap.map((a: ApBand, i) => `<tr data-k="ap" data-i="${i}"${isSel('ap', i)}>
        <td><input type="checkbox" data-f="on" ${a.on ? 'checked' : ''}/></td><td>${i + 1}</td>
        <td><select data-f="order">${opt('1', '1st', a.order === 1)}${opt('2', '2nd', a.order === 2)}</select></td>
        <td>${num('freq', a.freq, 1, 10, 24000, 62)}</td><td>${num('q', a.q, 0.05, 0.1, 10, 46)}</td>
        <td><button data-act="del">×</button></td></tr>`).join('')}
      </table>
      <div class="row"><button data-act="addAp" ${o.ap.length >= MAX_AP ? 'disabled' : ''}>+ all-pass</button></div>

      <h2>FIR</h2>
      <div class="row" data-k="fir"><label><input type="checkbox" data-f="on" ${o.fir.on ? 'checked' : ''} ${o.fir.taps ? '' : 'disabled'}/> on</label>
        <span class="grow dim" title="${esc(o.fir.path)}">${o.fir.taps ? esc(o.fir.path.split('/').pop() ?? '') : 'none loaded'}</span></div>
      ${o.fir.taps ? `<div class="dim">${o.fir.taps} taps · latency ${o.fir.latencySamples} smp = ${samplesToMs(o.fir.latencySamples, sr).toFixed(2)} ms</div>` : ''}
      <div class="row"><button data-act="loadFir">Load WAV / TXT…</button><button data-act="clearFir" ${o.fir.taps ? '' : 'disabled'}>Clear</button>
        <input type="file" data-ui="firFile" accept=".wav,.aif,.aiff,.txt,.csv" hidden/></div>
      <div class="dim">The FIR is applied after the IIR filters. It is measured, not drawn: watch the live TF.</div>`;
  }

  // Refresh numbers in place while dragging (no rebuild: keeps focus and scroll).
  private updatePanelValues(): void {
    const o = this.cfg;
    if (!o) return;
    this.side.querySelectorAll<HTMLTableRowElement>('tr[data-k]').forEach((tr) => {
      const item = tr.dataset.k === 'peq' ? o.peq[+tr.dataset.i!] : o.ap[+tr.dataset.i!];
      if (!item) return;
      tr.querySelectorAll<HTMLInputElement>('input[type=number]').forEach((inp) => {
        if (document.activeElement !== inp) inp.value = String(+(item as unknown as Record<string, number>)[inp.dataset.f!].toFixed(3));
      });
    });
    this.side.querySelectorAll<HTMLElement>('div[data-k=hp], div[data-k=lp]').forEach((row) => {
      const inp = row.querySelector<HTMLInputElement>('input[data-f=freq]');
      if (inp && document.activeElement !== inp) inp.value = String(o[row.dataset.k as 'hp' | 'lp'].freq);
    });
  }

  private onPanelChange(e: Event): void {
    const el = e.target as HTMLInputElement | HTMLSelectElement;
    const o = this.cfg;
    if (!o) return;
    if (el.dataset.ui === 'base') { this.baseName = el.value; this.redraw(); return; }
    if (el.dataset.ui === 'live') { this.showLive = (el as HTMLInputElement).checked; this.redraw(); return; }
    if (el.dataset.ui === 'linked') { this.linked = (el as HTMLInputElement).checked; return; }
    if (el.dataset.ui === 'firFile') { this.loadFir(el as HTMLInputElement); return; }
    const f = el.dataset.f;
    const holder = el.closest<HTMLElement>('[data-k]');
    if (!f || !holder) return;
    const value = el.type === 'checkbox' ? (el as HTMLInputElement).checked : el.type === 'number' ? Number(el.value) : el.value;
    const k = holder.dataset.k!;
    if (k === 'peq' || k === 'ap') {
      const list = (k === 'peq' ? o.peq : o.ap) as unknown as Record<string, unknown>[];
      list[+holder.dataset.i!][f] = f === 'order' ? Number(value) : value;
      if (k === 'peq') this.sendPeq(); else this.sendAp();
      this.selected = { kind: k, index: +holder.dataset.i! };
    } else if (k === 'hp' || k === 'lp') {
      (o[k] as unknown as Record<string, unknown>)[f] = value;
      this.send(`${k}/${f}`, value);
    } else if (k === 'fir') {
      o.fir.on = Boolean(value);
      this.send('fir/on', value);
    }
    this.redraw();
  }

  private onPanelClick(e: Event): void {
    const el = e.target as HTMLElement;
    const act = el.dataset.act;
    const o = this.cfg;
    if (!o) return;
    const tr = el.closest<HTMLElement>('tr[data-k]');
    if (tr && !act) { this.selected = { kind: tr.dataset.k as 'peq' | 'ap', index: +tr.dataset.i! }; this.redraw(); return; }
    if (act === 'del' && tr) { this.removeHandle({ kind: tr.dataset.k as 'peq' | 'ap', index: +tr.dataset.i! }); return; }
    if (act === 'addPeq' && o.peq.length < MAX_PEQ) {
      o.peq.push({ on: true, type: 'bell', freq: 1000, gainDb: 0, q: 1.4 });
      this.selected = { kind: 'peq', index: o.peq.length - 1 };
      this.sendPeq();
      this.render();
    }
    if (act === 'addAp' && o.ap.length < MAX_AP) {
      o.ap.push({ on: true, order: 2, freq: 100, q: 0.7 });
      this.selected = { kind: 'ap', index: o.ap.length - 1 };
      this.sendAp();
      this.render();
    }
    if (act === 'loadFir') this.side.querySelector<HTMLInputElement>('[data-ui=firFile]')!.click();
    if (act === 'clearFir') this.engine.request('fir.clear', { output: this.out }).catch((err: Error) => alert(err.message));
  }

  private loadFir(input: HTMLInputElement): void {
    const file = input.files?.[0];
    input.value = '';
    if (!file) return;
    const path = window.vt?.pathForFile(file) ?? prompt('Full path of the FIR file on the engine machine:') ?? '';
    if (!path) return;
    const outs = this.linked ? [...new Set([this.out, ...this.selection()])] : [this.out];
    Promise.all(outs.map((o) => this.engine.request('fir.load', { output: o, path }))).catch((err: Error) => alert(err.message));
  }
}

