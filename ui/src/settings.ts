import './lib/style.css';
import { connectFromLocation } from './lib/engine';
import { mountStatusBar, esc } from './lib/statusbar';
import { Channels } from './lib/channels';
import { dbToLin, linToDb, samplesToMs } from './lib/scale';
import type { SystemState } from './lib/types';

const engine = connectFromLocation();
mountStatusBar(document.getElementById('status')!, engine);
const channels = new Channels(engine);
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const report = (p: Promise<unknown>) => p.catch((e: Error) => alert(e.message));
const opt = (v: string | number, label: string, sel: boolean, disabled = false) =>
  `<option value="${esc(String(v))}"${sel ? ' selected' : ''}${disabled ? ' disabled' : ''}>${esc(label)}</option>`;
const OUTPUT_TYPES = ['full', 'main', 'sub', 'front-fill', 'out-fill', 'delay', 'monitor'];

// ---------------------------------------------------------------- tabs (remembered per viewer)
function showTab(name: string): void {
  document.querySelectorAll<HTMLElement>('[data-page]').forEach((p) => (p.hidden = p.dataset.page !== name));
  document.querySelectorAll<HTMLElement>('[data-tab]').forEach((b) => b.classList.toggle('on', b.dataset.tab === name));
  try { localStorage.setItem('vt.settingsTab', name); } catch { /* storage unavailable */ }
  if (name === 'audio') void loadLatency();
}
$('tabs').onclick = (e) => { const t = (e.target as HTMLElement).dataset.tab; if (t) showTab(t); };
let initialTab = 'inputs';
try { initialTab = localStorage.getItem('vt.settingsTab') ?? 'inputs'; } catch { /* default */ }
if (!document.querySelector(`[data-page=${initialTab}]`)) initialTab = 'inputs';
showTab(initialTab);

// ---------------------------------------------------------------- audio device
interface DeviceList { inputs: string[]; outputs: string[]; sampleRates: number[]; bufferSizes: number[] }

async function loadDevices(): Promise<void> {
  const d = await engine.request<DeviceList>('devices.list');
  const s = engine.state, st = engine.status;
  const curOut = s?.device.outputDevice || st?.outputDevice || '';
  const curIn = s?.device.inputDevice || st?.inputDevice || '';
  ($('outDev') as HTMLSelectElement).innerHTML = d.outputs.map((n) => opt(n, n, n === curOut)).join('');
  ($('inDev') as HTMLSelectElement).innerHTML = opt('', '— none —', !curIn) + d.inputs.map((n) => opt(n, n, n === curIn)).join('');
  const rates = d.sampleRates.length ? d.sampleRates : [44100, 48000, 88200, 96000];
  const bufs = d.bufferSizes.length ? d.bufferSizes : [32, 64, 128, 256, 512];
  ($('rate') as HTMLSelectElement).innerHTML = rates.map((r) => opt(r, `${r} Hz`, r === (s?.device.sampleRate ?? 48000))).join('');
  ($('buf') as HTMLSelectElement).innerHTML = bufs.map((b) => opt(b, `${b}`, b === (s?.device.bufferSize ?? 64))).join('');
}

$('apply').onclick = () => report(engine.request('device.open', {
  outputDevice: ($('outDev') as HTMLSelectElement).value,
  inputDevice: ($('inDev') as HTMLSelectElement).value,
  sampleRate: Number(($('rate') as HTMLSelectElement).value),
  bufferSize: Number(($('buf') as HTMLSelectElement).value),
}).then(loadDevices).then(loadLatency));
$('rescan').onclick = () => report(loadDevices());

interface LatRow { direction: string; device: string; deviceLatency: number; safetyOffset: number; streamLatency: number; buffer: number }
async function loadLatency(): Promise<void> {
  const rows = await engine.request<LatRow[]>('device.latency').catch(() => [] as LatRow[]);
  const sr = engine.status?.sampleRate || 48000;
  const ms = (n: number) => samplesToMs(n, sr).toFixed(2);
  if (!rows.length) { $('latTable').innerHTML = '<tr><td class="dim">Not available for this device / platform.</td></tr>'; return; }
  let total = 0, hw = 0;
  $('latTable').innerHTML = `<tr><th>direction</th><th>device</th><th>device latency</th><th>safety offset</th><th>stream (hardware DSP)</th><th>buffer (VenueTuner)</th><th>total</th></tr>` +
    rows.map((r) => {
      const t = r.deviceLatency + r.safetyOffset + r.streamLatency + r.buffer;
      total += t; hw += t - r.buffer;
      return `<tr><td>${r.direction}</td><td>${esc(r.device)}</td><td class="num">${ms(r.deviceLatency)} ms</td><td class="num">${ms(r.safetyOffset)} ms</td>
        <td class="num ${r.streamLatency > sr * 0.005 ? 'warn' : ''}">${ms(r.streamLatency)} ms</td><td class="num">${ms(r.buffer)} ms</td><td class="num"><b>${ms(t)} ms</b></td></tr>`;
    }).join('') + `<tr><td colspan="6" class="num">round trip</td><td class="num"><b>${ms(total)} ms</b></td></tr>`;
  $('latNote').innerHTML = `Driver and hardware: <b>${ms(hw)} ms</b> · VenueTuner buffers: <b>${ms(total - hw)} ms</b>. ` +
    (hw > total * 0.7 ? 'Most of this latency is inside the device (e.g. built-in Mac mic/speaker DSP) and cannot be reduced in software: use a pro USB/Thunderbolt interface for the live path.' : 'Lower the buffer size if the system stays free of xruns.');
}

// ---------------------------------------------------------------- inputs & outputs pages
function chanOptions(names: string[], current: number, usedBy: Map<number, string> = new Map()): string {
  return opt(-1, '— not connected —', current < 0) + names.map((n, i) => {
    const user = i === current ? undefined : usedBy.get(i); // only flag channels taken by *other* outputs
    return opt(i, `${i + 1} · ${n}${user ? `  (used by ${user})` : ''}`, i === current, !!user && i !== current);
  }).join('');
}

function renderInputs(s: SystemState): void {
  $('inputs').innerHTML = `<tr><th>#</th><th>name</th><th>type</th><th>interface channel</th><th>level</th><th></th></tr>` +
    (s.sources.map((src, i) => `<tr>
      <td>${i + 1}</td>
      <td><input type="text" data-p="sources/${i}/name" value="${esc(src.name)}"/></td>
      <td><select data-p="sources/${i}/type">${opt('program', 'program', src.type === 'program')}${opt('mic', 'measurement mic', src.type === 'mic')}${opt('loopback', 'loopback', src.type === 'loopback')}</select></td>
      <td><select data-p="sources/${i}/channel" data-int="1">${chanOptions(channels.inputs, src.channel)}</select></td>
      <td><div class="meter"><div data-in-meter="${src.channel}"></div></div></td>
      <td><button data-rm-src="${i}" title="remove">×</button></td></tr>`).join('') ||
      '<tr><td colspan="6" class="dim">No inputs yet: add your console feeds and your measurement mic.</td></tr>');
}

// "Fed by" chips per output instead of a matrix: read left to right.
function fedBy(s: SystemState, oi: number): string {
  const o = s.outputs[oi];
  const chips = s.sources.map((src, i) => ({ src, i, g: o.mix[i] ?? 0 }))
    .filter(({ src, g }) => g > 0 && src.type === 'program')
    .map(({ src, i, g }) => `<span class="chip">${esc(src.name)}
      <input type="number" step="0.5" min="-40" max="12" data-mixdb="${oi}:${i}" value="${+linToDb(g).toFixed(1)}"/><span class="dim">dB</span>
      <button data-unmix="${oi}:${i}" title="remove">×</button></span>`).join('');
  const free = s.sources.map((src, i) => ({ src, i })).filter(({ src, i }) => src.type === 'program' && !((o.mix[i] ?? 0) > 0));
  const add = free.length ? `<select class="add-in" data-addmix="${oi}"><option value="">+ add input…</option>${free.map(({ src, i }) => opt(i, src.name, false)).join('')}</select>` : '';
  return chips + add || '<span class="dim">no program inputs yet</span>';
}

function renderOutputs(s: SystemState): void {
  const usedBy = new Map<number, string>();
  s.outputs.forEach((o) => { if (o.channel >= 0) usedBy.set(o.channel, o.name); });
  $('outputs').innerHTML = `<tr><th>#</th><th></th><th>name</th><th>type</th><th>interface channel</th><th>fed by</th><th></th></tr>` +
    (s.outputs.map((o, i) => `<tr>
      <td>${i + 1}</td>
      <td><input type="color" class="swatch-in" data-p="outputs/${i}/colour" value="${esc(o.colour)}"/></td>
      <td><input type="text" data-p="outputs/${i}/name" value="${esc(o.name)}"/></td>
      <td><select data-p="outputs/${i}/type">${OUTPUT_TYPES.map((t) => opt(t, t, t === o.type)).join('')}</select></td>
      <td><select data-p="outputs/${i}/channel" data-int="1">${chanOptions(channels.outputs, o.channel, usedBy)}</select></td>
      <td class="fed">${fedBy(s, i)}</td>
      <td><button data-mv="${i}:${i - 1}" ${i === 0 ? 'disabled' : ''} title="move up">↑</button><button data-mv="${i}:${i + 1}" ${i === s.outputs.length - 1 ? 'disabled' : ''} title="move down">↓</button>
        <button data-dup="${i}" title="duplicate (with processing)">⧉</button><button data-rm-out="${i}" title="remove">×</button></td></tr>`).join('') ||
      '<tr><td colspan="7" class="dim">No outputs yet: add one per speaker feed you need.</td></tr>');
}

function renderPatch(s: SystemState): void {
  const a = document.activeElement;
  // Don't rebuild under the user's cursor while typing a name or a level.
  if (a && a.matches('[data-page=inputs] input, [data-page=outputs] input:not([type=color])')) return;
  renderInputs(s);
  renderOutputs(s);
}

for (const pageName of ['inputs', 'outputs']) {
  const page = document.querySelector<HTMLElement>(`[data-page=${pageName}]`)!;
  page.addEventListener('change', (e) => {
    const el = e.target as HTMLInputElement | HTMLSelectElement;
    const d = el.dataset;
    if (d.p) report(engine.patch(d.p, d.int ? Number(el.value) : el.value));
    if (d.mixdb) { const [o, i] = d.mixdb.split(':').map(Number); report(engine.patch(`outputs/${o}/mix/${i}`, dbToLin(Math.max(-40, Math.min(12, Number(el.value)))))); }
    if (d.addmix && el.value !== '') report(engine.patch(`outputs/${d.addmix}/mix/${el.value}`, 1));
  });
  page.addEventListener('focusout', () => setTimeout(() => engine.state && renderPatch(engine.state), 0));
  page.addEventListener('click', (e) => {
    const d = (e.target as HTMLElement).dataset;
    if (d.addSrc) report(engine.request('routing.addSource', { type: d.addSrc }));
    if (d.addOut) report(engine.request('routing.addOutput', { preset: d.addOut }));
    if (d.rmSrc && confirm('Remove this input? It is also removed from every output it feeds.')) report(engine.request('routing.removeSource', { index: +d.rmSrc }));
    if (d.rmOut && confirm('Remove this output and its processing?')) report(engine.request('routing.removeOutput', { index: +d.rmOut }));
    if (d.dup) report(engine.request('routing.duplicateOutput', { index: +d.dup }));
    if (d.mv) { const [a, b] = d.mv.split(':').map(Number); report(engine.request('routing.moveOutput', { index: a, to: b })); }
    if (d.unmix) { const [o, i] = d.unmix.split(':').map(Number); report(engine.patch(`outputs/${o}/mix/${i}`, 0)); }
  });
}

// Input meters on the Inputs page.
engine.on('status', (st) => {
  document.querySelectorAll<HTMLElement>('[data-in-meter]').forEach((m) => {
    const ch = Number(m.dataset.inMeter);
    const db = ch >= 0 ? linToDb(st.inPeak[ch] ?? 0) : -Infinity;
    m.style.width = `${Math.max(0, Math.min(100, ((db + 60) / 60) * 100))}%`;
  });
});

// ---------------------------------------------------------------- calibration
function renderCal(s: SystemState): void {
  const co = $('calOut') as HTMLSelectElement, ci = $('calIn') as HTMLSelectElement;
  const [po, pi] = [co.value, ci.value];
  co.innerHTML = s.outputs.map((o, i) => opt(i, `${o.name} (${channels.outLabel(o.channel)})`, String(i) === po)).join('');
  ci.innerHTML = channels.inputs.map((n, i) => opt(i, `${i + 1} · ${n}`, String(i) === pi)).join('');
  if (document.activeElement !== $('micPath')) ($('micPath') as HTMLInputElement).value = s.cal.micCalPath;
}

function renderLatencyInfo(): void {
  const st = engine.status;
  if (!st) return;
  $('latInfo').innerHTML = `current: <b>${st.ioLatency.toFixed(1)} samples · ${samplesToMs(st.ioLatency, st.sampleRate || 48000).toFixed(3)} ms</b>
    <span class="dim">(${st.ioLatencyCalibrated ? 'calibrated' : 'driver-reported'}; driver says ${st.driverLatency})</span>${st.calibrating ? ' <span class="warn">calibrating…</span>' : ''}`;
}

$('calRun').onclick = () => {
  if (!confirm('Pink noise will play at -20 dBFS on the selected output. Make sure it is cabled to the selected input and NOT connected to speakers. Continue?')) return;
  $('calResult').textContent = 'measuring…';
  report(engine.request('calibrate.latency', { output: Number(($('calOut') as HTMLSelectElement).value), input: Number(($('calIn') as HTMLSelectElement).value) }));
};
$('calClear').onclick = () => report(engine.request('calibrate.clear'));
engine.on('calibration', (c) => {
  $('calResult').innerHTML = c.ok ? `<span class="ok">${esc(c.message)}: ${c.samples.toFixed(2)} samples</span>` : `<span class="bad">${esc(c.message)}</span>`;
});
$('micBrowse').onclick = () => $('micFile').click();
$('micFile').onchange = (e) => {
  const f = (e.target as HTMLInputElement).files?.[0];
  if (!f) return;
  const path = window.vt?.pathForFile(f) ?? '';
  if (!path) return alert('File paths are only available in the desktop app: type the path instead.');
  ($('micPath') as HTMLInputElement).value = path;
  report(engine.request('cal.loadMic', { path }));
};
$('micLoad').onclick = () => report(engine.request('cal.loadMic', { path: ($('micPath') as HTMLInputElement).value.trim() }));
$('micClear').onclick = () => report(engine.request('cal.clearMic'));

// ---------------------------------------------------------------- wiring
const renderAll = () => { if (engine.state) { renderPatch(engine.state); renderCal(engine.state); } };
engine.on('state', renderAll);
channels.onChange(renderAll);
engine.on('status', renderLatencyInfo);
engine.on('open', () => void loadDevices().then(loadLatency).catch(() => undefined));
