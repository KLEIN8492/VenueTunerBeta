// Minimal canvas plot: log or linear X, grid, multiple traces, optional per-point
// opacity, phase-wrap breaks and a hover cursor readout.
import { freqTicks, fmtHz, invLogX, linX, linY, logX } from './scale';

export interface PlotTrace {
  x: ArrayLike<number>;
  y: ArrayLike<number>;
  color: string;
  width?: number;
  alpha?: ArrayLike<number>; // per-point opacity (e.g. from coherence)
  wrap?: number;             // break the line where |Δy| exceeds this (phase)
  label?: string;
}

export interface PlotOptions {
  xMin: number; xMax: number; logX: boolean;
  yMin: number; yMax: number; yStep: number;
  yUnit: string; title: string;
  xFormat?: (x: number) => string;
  yLocked?: boolean; // ignore wheel panning (e.g. coherence)
}

const css = (name: string) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

export class Plot {
  traces: PlotTrace[] = [];
  markers: { x: number; color: string }[] = [];
  // Drawn after the traces, in plot-area coordinates (use toPx).
  onOverlay?: (ctx: CanvasRenderingContext2D) => void;

  // Plot-area geometry from the current canvas size (no dependency on a drawn frame).
  get geom() {
    const padL = 44, padR = 14, padB = 16, padT = 14;
    return { padL, padT, W: Math.max(1, this.w - padL - padR), H: Math.max(1, this.h - padB - padT) };
  }
  private ctx: CanvasRenderingContext2D;
  private w = 0; private h = 0;
  private hoverX: number | null = null;
  private queued = false;

  constructor(private canvas: HTMLCanvasElement, public opts: PlotOptions) {
    this.ctx = canvas.getContext('2d')!;
    new ResizeObserver(() => this.resize()).observe(canvas);
    canvas.addEventListener('mousemove', (e) => { this.hoverX = e.offsetX; this.draw(); });
    canvas.addEventListener('mouseleave', () => { this.hoverX = null; this.draw(); });
    canvas.addEventListener('wheel', (e) => {
      if (this.opts.yLocked) return;
      e.preventDefault();
      const d = Math.sign(e.deltaY) * this.opts.yStep;
      this.opts.yMin += d; this.opts.yMax += d;
      this.draw();
    }, { passive: false });
    canvas.addEventListener('dblclick', () => this.onReset?.());
    this.resize();
  }

  onReset?: () => void;

  private resize(): void {
    const dpr = window.devicePixelRatio || 1;
    const r = this.canvas.getBoundingClientRect();
    this.w = r.width; this.h = r.height;
    this.canvas.width = Math.max(1, Math.round(r.width * dpr));
    this.canvas.height = Math.max(1, Math.round(r.height * dpr));
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.draw();
  }

  // Coalesce redraws to one per animation frame.
  draw(): void {
    if (this.queued) return;
    this.queued = true;
    requestAnimationFrame(() => { this.queued = false; this.render(); });
  }

  // Data → plot-area pixels, and canvas offset pixels → data.
  toPx(x: number, y: number): [number, number] {
    const { W, H } = this.geom;
    return [this.px(x, W), linY(y, this.opts.yMin, this.opts.yMax, H)];
  }
  fromOffset(offsetX: number, offsetY: number): [number, number] {
    const { padL, padT, W, H } = this.geom, o = this.opts;
    const px = offsetX - padL, py = offsetY - padT;
    const x = o.logX ? invLogX(px, o.xMin, o.xMax, W) : o.xMin + (px / W) * (o.xMax - o.xMin);
    return [x, o.yMax - (py / H) * (o.yMax - o.yMin)];
  }

  private px(x: number, width: number): number {
    const o = this.opts;
    return o.logX ? logX(x, o.xMin, o.xMax, width) : linX(x, o.xMin, o.xMax, width);
  }

  private render(): void {
    const { ctx, opts: o } = this;
    if (this.w <= 60 || this.h <= 32) return;
    const { padL, padT, W, H } = this.geom;
    ctx.clearRect(0, 0, this.w, this.h);
    ctx.font = `10px ${css('--mono')}`;
    ctx.textBaseline = 'middle';

    // Grid
    ctx.save();
    ctx.translate(padL, padT);
    ctx.lineWidth = 1;
    const fmtX = o.xFormat ?? fmtHz;
    if (o.logX) {
      for (const t of freqTicks(o.xMin, o.xMax)) {
        const x = Math.round(this.px(t.f, W)) + 0.5;
        ctx.strokeStyle = t.major ? css('--grid-major') : css('--grid');
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
        if (t.major) { ctx.fillStyle = css('--dim'); ctx.textAlign = 'center'; ctx.fillText(fmtX(t.f), x, H + 9); }
      }
    } else {
      const span = o.xMax - o.xMin;
      const step = Math.pow(10, Math.floor(Math.log10(span / 5)));
      const s = span / step > 10 ? step * 2 : step;
      for (let v = Math.ceil(o.xMin / s) * s; v <= o.xMax; v += s) {
        const x = Math.round(this.px(v, W)) + 0.5;
        ctx.strokeStyle = css('--grid');
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
        ctx.fillStyle = css('--dim'); ctx.textAlign = 'center'; ctx.fillText(fmtX(v), x, H + 9);
      }
    }
    for (let v = Math.ceil(o.yMin / o.yStep) * o.yStep; v <= o.yMax + 1e-9; v += o.yStep) {
      const y = Math.round(linY(v, o.yMin, o.yMax, H)) + 0.5;
      ctx.strokeStyle = Math.abs(v) < 1e-9 ? css('--grid-major') : css('--grid');
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
      ctx.fillStyle = css('--dim'); ctx.textAlign = 'right'; ctx.fillText(`${+v.toFixed(2)}`, -4, y);
    }
    ctx.fillStyle = css('--fg'); ctx.textAlign = 'left';
    ctx.fillText(`${o.title} [${o.yUnit}]`, 4, -7);

    // Traces (clipped to the plot area)
    ctx.beginPath(); ctx.rect(0, 0, W, H); ctx.clip();
    for (const m of this.markers) {
      const x = this.px(m.x, W);
      ctx.strokeStyle = m.color; ctx.setLineDash([3, 3]);
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
      ctx.setLineDash([]);
    }
    for (const t of this.traces) this.drawTrace(t, W, H);
    this.onOverlay?.(ctx);
    ctx.restore();

    // Hover readout
    if (this.hoverX !== null && this.hoverX > padL && this.hoverX < padL + W) {
      const xv = o.logX ? invLogX(this.hoverX - padL, o.xMin, o.xMax, W) : o.xMin + ((this.hoverX - padL) / W) * (o.xMax - o.xMin);
      ctx.strokeStyle = css('--dim');
      ctx.beginPath(); ctx.moveTo(this.hoverX + 0.5, padT); ctx.lineTo(this.hoverX + 0.5, padT + H); ctx.stroke();
      const parts = [fmtX(xv)];
      for (const t of this.traces) {
        const v = valueAt(t, xv);
        if (v !== null) parts.push(`${t.label ? t.label + ' ' : ''}${v.toFixed(o.yUnit === 'deg' ? 0 : 2)}`);
      }
      ctx.fillStyle = css('--fg'); ctx.textAlign = 'right';
      ctx.fillText(parts.join('  '), this.w - 4, padT - 7);
    }
  }

  private drawTrace(t: PlotTrace, W: number, H: number): void {
    const { ctx, opts: o } = this;
    const n = Math.min(t.x.length, t.y.length);
    ctx.strokeStyle = t.color;
    ctx.lineWidth = t.width ?? 1.25;
    const pts: [number, number][] = [];
    for (let i = 0; i < n; i++) pts.push([this.px(t.x[i], W), linY(t.y[i], o.yMin, o.yMax, H)]);
    if (!t.alpha && !t.wrap) {
      ctx.beginPath();
      pts.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
      ctx.stroke();
      return;
    }
    for (let i = 0; i + 1 < n; i++) {
      if (t.wrap && Math.abs(t.y[i + 1] - t.y[i]) > t.wrap) continue;
      const a = t.alpha ? Math.min(t.alpha[i], t.alpha[i + 1]) : 1;
      if (a <= 0.01) continue;
      ctx.globalAlpha = a;
      ctx.beginPath(); ctx.moveTo(...pts[i]); ctx.lineTo(...pts[i + 1]); ctx.stroke();
    }
    ctx.globalAlpha = 1;
  }
}

function valueAt(t: PlotTrace, x: number): number | null {
  const n = Math.min(t.x.length, t.y.length);
  if (n === 0 || x < t.x[0] || x > t.x[n - 1]) return null;
  let i = 0;
  while (i + 1 < n && t.x[i + 1] < x) i++;
  return t.y[i];
}
