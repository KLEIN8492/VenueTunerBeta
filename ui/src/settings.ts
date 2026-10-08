import './lib/style.css';
import { connectFromLocation } from './lib/engine';
import { mountStatusBar, esc } from './lib/statusbar';
import { dbToLin, linToDb, samplesToMs } from './lib/scale';
import type { SystemState } from './lib/types';

declare global { interface Window { vt?: { pathForFile(f: File): string } } }

const engine = connectFromLocation();
mountStatusBar(document.getElementById('status')!, engine);
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const report = (p: Promise<unknown>) => p.catch((e: Error) => alert(e.message));
const opt = (v: string | number, label: string, sel: boolean) => `<option value="${esc(String(v))}"${sel ? ' selected' : ''}>${esc(label)}</option>`;
const OUTPUT_TYPES = ['full', 'main', 'sub', 'front-fill', 'out-fill', 'delay', 'monitor'];

// ---------------------------------------------------------------- device
interface DeviceList { inputs: string[]; outputs: string[]; sampleRates: number[]; bufferSizes: number[] }

async function loadDevices(): Promise<void> {
  const d = await engine.request<DeviceList>('devices.list');
  const s = engine.state;
  const st = engine.status;
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
}).then(loadDevices));
$('rescan').onclick = () => report(loadDevices());

// ---------------------------------------------------------------- tables
function render(s: SystemState): void {
  if (document.activeElement && document.activeElement.closest('table')) return; // don't fight the user's typing

  $('inputs').innerHTML = `<tr><th>#</th><th>Name</th><th>Role</th></tr>` + s.inputs.map((inp, i) => `
    <tr><td>${i + 1}</td><td><input type="text" data-p="inputs/${i}/name" value="${esc(inp.name)}"/></td>
    <td><select data-p="inputs/${i}/role">${['console', 'mic', 'loopback', 'none'].map((r) => opt(r, r, r === inp.role)).join('')}</select></td></tr>`).join('');

  $('outputs').innerHTML = `<tr><th>#</th><th>Name</th><th>Type</th><th>Colour</th></tr>` + s.outputs.map((o, i) => `
    <tr><td>${i + 1}</td><td><input type="text" data-p="outputs/${i}/name" value="${esc(o.name)}"/></td>
    <td><select data-p="outputs/${i}/type">${OUTPUT_TYPES.map((t) => opt(t, t, t === o.type)).join('')}</select></td>
    <td><input type="color" data-p="outputs/${i}/colour" value="${esc(o.colour)}"/></td></tr>`).join('');

  const consoleIns = s.inputs.map((inp, i) => ({ inp, i })).filter(({ inp }) => inp.role === 'console');
  $('matrix').innerHTML = !consoleIns.length ? '<tr><td class="dim">No inputs with role "console".</td></tr>' :
    `<tr><th></th>${consoleIns.map(({ inp, i }) => `<th class="vert">${i + 1} · ${esc(inp.name)}</th>`).join('')}</tr>` +
    s.outputs.map((o, oi) => `<tr><th>${oi + 1} · ${esc(o.name)}</th>${consoleIns.map(({ i }) => {
      const g = o.mix[i] ?? 0;
      return `<td class="${g > 0 ? 'on' : ''}"><input type="number" step="0.5" max="12" data-mix="${oi}:${i}" value="${g > 0 ? +linToDb(g).toFixed(2) : ''}"/></td>`;
    }).join('')}</tr>`).join('');

  const optsOut = s.outputs.map((o, i) => opt(i, `${i + 1} · ${o.name}`, false)).join('');
  const optsIn = s.inputs.map((o, i) => opt(i, `${i + 1} · ${o.name}`, false)).join('');
  const co = $('calOut') as HTMLSelectElement, ci = $('calIn') as HTMLSelectElement;
  const [po, pi] = [co.value, ci.value];
  co.innerHTML = optsOut; ci.innerHTML = optsIn;
  if (po) co.value = po;
  if (pi) ci.value = pi;
  ($('micPath') as HTMLInputElement).value = s.cal.micCalPath;
}

function renderLatency(): void {
  const st = engine.status;
  if (!st) return;
  $('latInfo').innerHTML = `current: <b>${st.ioLatency.toFixed(1)} samples · ${samplesToMs(st.ioLatency, st.sampleRate || 48000).toFixed(3)} ms</b>
    <span class="dim">(${st.ioLatencyCalibrated ? 'calibrated' : 'driver-reported'}; driver says ${st.driverLatency})</span>${st.calibrating ? ' <span class="warn">calibrating…</span>' : ''}`;
}

document.addEventListener('change', (e) => {
  const el = e.target as HTMLInputElement | HTMLSelectElement;
  if (el.dataset.p) report(engine.patch(el.dataset.p, el.value));
  if (el.dataset.mix) {
    const [o, i] = el.dataset.mix.split(':').map(Number);
    const v = el.value.trim();
    const lin = v === '' ? 0 : Math.min(4, dbToLin(Number(v)));
    report(engine.patch(`outputs/${o}/mix/${i}`, lin));
  }
});

// ---------------------------------------------------------------- calibration
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

engine.on('state', render);
engine.on('status', renderLatency);
engine.on('open', () => void loadDevices().catch(() => undefined));
