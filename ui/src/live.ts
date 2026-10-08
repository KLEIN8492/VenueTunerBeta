import './lib/style.css';
import { connectFromLocation } from './lib/engine';
import { mountStatusBar, esc } from './lib/statusbar';
import { linToDb, samplesToMs } from './lib/scale';
import { EqEditor } from './lib/eqeditor';
import { Channels } from './lib/channels';
import type { OutputConfig, Status, SystemState } from './lib/types';

const engine = connectFromLocation();
mountStatusBar(document.getElementById('status')!, engine);
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const report = (p: Promise<unknown>) => p.catch((e: Error) => alert(e.message));

const channels = new Channels(engine);
const selected = new Set<number>();
let builtFor = '';
const editor = new EqEditor($('editor'), engine, () => [...selected]);
if (import.meta.env.DEV) Object.assign(window, { vtEditor: editor }); // debugging aid

// ---------------------------------------------------------------- strips
// Strips are rebuilt only when the output list shape changes; values are updated in
// place so typing into a field is never interrupted by incoming state.
function stripHtml(o: OutputConfig, i: number): string {
  return `
  <div class="strip" data-i="${i}">
    <div class="name" data-sel="${i}" style="border-color:${esc(o.colour)}">${i + 1} · <span data-f="name"></span></div>
    <div class="patch" data-f="patch"></div>
    <div class="meters">
      <div class="scale"><span>0</span><span>-12</span><span>-24</span><span>-36</span><span>-48</span><span>-60</span></div>
      <div class="meter" title="output level"><div class="fill" data-m="out"></div></div>
      <div class="meter gr" title="limiter gain reduction"><div class="fill" data-m="gr"></div></div>
      <div class="vals"><div data-v="peak"></div><div data-v="gr"></div></div>
    </div>
    <div class="row"><button data-act="mute" class="mute grow">Mute</button><button data-act="pol" title="polarity">Ø</button></div>
    <div class="row"><label>Gain</label><input type="number" step="0.1" min="-80" max="12" data-k="gainDb"/><span class="dim">dB</span></div>
    <div class="row"><label>Delay</label><input type="number" step="0.01" min="0" max="1000" data-k="delayMs"/><span class="dim">ms</span></div>
    <div class="row"><button data-act="lim" class="grow">Limiter</button></div>
    <div class="row"><label>Thr</label><input type="number" step="0.5" min="-40" max="0" data-k="limThreshDb"/><span class="dim">dB</span></div>
    <div class="row"><label>Rel</label><input type="number" step="10" min="1" max="5000" data-k="limReleaseMs"/><span class="dim">ms</span></div>
    <div class="vals" data-v="lat"></div>
  </div>`;
}

function renderStrips(s: SystemState): void {
  const shape = s.outputs.map((o) => o.colour).join(',');
  const root = $('strips');
  if (shape !== builtFor) {
    root.innerHTML = s.outputs.map(stripHtml).join('') || '<div class="dim" style="padding:10px">No outputs yet: create them in Settings › Outputs (⌘,)</div>';
    builtFor = shape;
  }
  s.outputs.forEach((o, i) => {
    const el = root.querySelector<HTMLElement>(`.strip[data-i="${i}"]`);
    if (!el) return;
    el.classList.toggle('sel', selected.has(i));
    el.querySelector('[data-f="name"]')!.textContent = o.name;
    const patch = el.querySelector<HTMLElement>('[data-f="patch"]')!;
    patch.textContent = `→ ${channels.outLabel(o.channel)}`;
    patch.classList.toggle('none', o.channel < 0);
    el.querySelector('[data-act="mute"]')!.classList.toggle('on', o.mute);
    el.querySelector('[data-act="pol"]')!.classList.toggle('on', o.polarityInvert);
    el.querySelector('[data-act="lim"]')!.classList.toggle('on', o.limiterOn);
    for (const inp of el.querySelectorAll<HTMLInputElement>('input[data-k]')) {
      if (document.activeElement === inp) continue;
      inp.value = String(o[inp.dataset.k as keyof OutputConfig]);
    }
    renderLatency(el, o);
  });
}

function renderLatency(el: HTMLElement, o: OutputConfig): void {
  const st = engine.status;
  if (!st) return;
  const sr = st.sampleRate || 48000;
  const io = samplesToMs(st.ioLatency, sr);
  const fir = o.fir?.on ? samplesToMs(o.fir.latencySamples, sr) : 0;
  el.querySelector('[data-v="lat"]')!.textContent =
    `latency ${(io + o.delayMs + fir).toFixed(2)} ms` + (fir ? ` (FIR ${fir.toFixed(2)})` : '');
}

const meterPct = (db: number) => Math.max(0, Math.min(100, ((db + 60) / 60) * 100));

function renderMeters(st: Status): void {
  const root = $('strips');
  st.outPeak.forEach((pk, i) => {
    const el = root.querySelector<HTMLElement>(`.strip[data-i="${i}"]`);
    if (!el) return;
    const db = linToDb(pk), gr = st.limGr[i] ?? 0;
    const fill = el.querySelector<HTMLElement>('[data-m="out"]')!;
    fill.style.height = `${meterPct(db)}%`;
    fill.classList.toggle('hot', db > -6);
    fill.classList.toggle('clip', db > -0.1);
    el.querySelector<HTMLElement>('[data-m="gr"]')!.style.height = `${Math.min(100, (-gr / 20) * 100)}%`;
    el.querySelector('[data-v="peak"]')!.textContent = isFinite(db) ? db.toFixed(1) : '-∞';
    el.querySelector('[data-v="gr"]')!.textContent = gr < -0.1 ? `GR ${gr.toFixed(1)}` : '';
  });
}

// Which outputs an edit applies to: the clicked one, or every selected one with ⇧.
const targetsFor = (i: number, shift: boolean) => (shift && selected.has(i) ? [...selected] : [i]);

$('strips').addEventListener('click', (e) => {
  const el = e.target as HTMLElement;
  const strip = el.closest<HTMLElement>('.strip');
  if (!strip || !engine.state) return;
  const i = Number(strip.dataset.i);
  if (el.closest('[data-sel]')) {
    if (!e.metaKey && !e.shiftKey) selected.clear();
    if (selected.has(i) && selected.size > 1) selected.delete(i); else selected.add(i);
    editor.setOutput(i);
    renderStrips(engine.state);
    return;
  }
  const act = el.dataset.act;
  if (!act) return;
  const key = act === 'mute' ? 'mute' : act === 'pol' ? 'polarityInvert' : 'limiterOn';
  const value = !engine.state.outputs[i][key];
  report(engine.patchMany(targetsFor(i, e.shiftKey).map((o) => ({ path: `outputs/${o}/${key}`, value }))));
});

$('strips').addEventListener('change', (e) => {
  const inp = e.target as HTMLInputElement;
  const k = inp.dataset.k;
  const strip = inp.closest<HTMLElement>('.strip');
  if (!k || !strip) return;
  const i = Number(strip.dataset.i);
  const value = Number(inp.value);
  const shift = (e as unknown as { shiftKey?: boolean }).shiftKey || shiftHeld;
  report(engine.patchMany(targetsFor(i, shift).map((o) => ({ path: `outputs/${o}/${k}`, value }))));
});
let shiftHeld = false;
document.addEventListener('keydown', (e) => { if (e.key === 'Shift') shiftHeld = true; });
document.addEventListener('keyup', (e) => { if (e.key === 'Shift') shiftHeld = false; });

// ---------------------------------------------------------------- mute / fault
$('muteAll').onclick = () => engine.status && report(engine.request('mute.all', { on: !engine.status.muteAll }));

function renderBanner(st: Status): void {
  $('muteAll').className = st.muteAll ? 'danger' : '';
  $('muteAll').textContent = st.muteAll ? 'MUTED — CLICK TO UNMUTE' : 'MUTE ALL';
  const b = $('banner');
  const want = st.faultMuted ? 'fault' : '';
  if (b.dataset.state === want) return;
  b.dataset.state = want;
  b.innerHTML = st.faultMuted
    ? `<div class="banner">OUTPUTS MUTED — start-up, device change or device fault. Check levels, then <button id="unmute">Unmute outputs</button></div>`
    : '';
  if (st.faultMuted) $('unmute').onclick = () => report(engine.request('unmute.fault'));
}

// ---------------------------------------------------------------- snapshots
$('snap').innerHTML = `
  <h2>Snapshots</h2>
  <div class="row"><input id="snapName" type="text" class="grow" placeholder="snapshot name"/><button id="snapSave">Save</button></div>
  <div id="snapList" class="list"></div>
  <div class="row"><label>A/B</label><select id="snapA" class="grow"></select><select id="snapB" class="grow"></select></div>
  <div class="row"><button id="snapAB" class="grow">Toggle A ⇄ B</button></div>
  <div class="row"><button id="snapImport" class="grow">Import…</button><input id="snapFile" type="file" accept=".json" hidden/></div>
  <div class="dim" style="margin-top:8px">Snapshots store the patch (inputs, outputs, channels), routing and all processing. Recall crossfades gains and delays. On another interface you are asked to remap the channels.</div>
`;
let names: string[] = [];
let lastRecalled = '';

async function refreshSnapshots(): Promise<void> {
  names = await engine.request<string[]>('snapshot.list');
  $('snapList').innerHTML = names.map((n) => `
    <div class="item"><span class="grow">${n === lastRecalled ? '▶ ' : ''}${esc(n)}</span>
      <button data-recall="${esc(n)}">Recall</button><button data-export="${esc(n)}">⤓</button><button data-delete="${esc(n)}">×</button></div>`).join('')
    || '<div class="item dim">no snapshots</div>';
  for (const id of ['snapA', 'snapB']) {
    const sel = $(id) as HTMLSelectElement, prev = sel.value;
    sel.innerHTML = names.map((n) => `<option${n === prev ? ' selected' : ''}>${esc(n)}</option>`).join('');
  }
}

interface RemapItem { name: string; type?: string; was: number; channel: number }
interface RecallReply { needsRemap?: boolean; fromInput?: string; fromOutput?: string; sources?: RemapItem[]; outputs?: RemapItem[] }

async function recall(name: string): Promise<void> {
  const r = await engine.request<RecallReply>('snapshot.recall', { name });
  if (r?.needsRemap) { showRemap(name, r); return; }
  lastRecalled = name;
  await refreshSnapshots();
}

// The snapshot was made on another interface: choose where each input/output goes now.
function showRemap(name: string, r: RecallReply): void {
  const chan = (names: string[], cur: number) =>
    `<option value="-1"${cur < 0 ? ' selected' : ''}>— not connected —</option>` +
    names.map((n, i) => `<option value="${i}"${i === cur ? ' selected' : ''}>${i + 1} · ${esc(n)}</option>`).join('');
  const rows = (items: RemapItem[], names: string[], kind: string) => items.map((x, i) =>
    `<tr><td>${esc(x.name)}</td><td class="dim">was ${x.was < 0 ? '—' : x.was + 1}</td><td><select data-${kind}="${i}">${chan(names, x.channel)}</select></td></tr>`).join('');
  $('modal').innerHTML = `<div class="modal"><div>
    <h2>Snapshot "${esc(name)}" was made on another interface</h2>
    <div class="dim">From: ${esc(r.fromOutput ?? '?')}${r.fromInput && r.fromInput !== r.fromOutput ? ' / ' + esc(r.fromInput) : ''}. Choose the channels on the current interface (prefilled by name, then by number).</div>
    <h2>Inputs</h2><table>${rows(r.sources ?? [], channels.inputs, 'rs')}</table>
    <h2>Outputs</h2><table>${rows(r.outputs ?? [], channels.outputs, 'ro')}</table>
    <div class="row" style="margin-top:10px"><button id="remapApply" class="on">Recall with this patch</button><button id="remapCancel">Cancel</button></div>
  </div></div>`;
  $('remapCancel').onclick = () => { $('modal').innerHTML = ''; };
  $('remapApply').onclick = () => {
    const pick = (kind: string) => [...document.querySelectorAll<HTMLSelectElement>(`[data-${kind}]`)].map((s) => Number(s.value));
    const outs = pick('ro').filter((c) => c >= 0);
    if (new Set(outs).size !== outs.length) return alert('Two outputs are on the same interface channel.');
    report(engine.request('snapshot.recall', { name, remap: { sources: pick('rs'), outputs: pick('ro') } }).then(async () => {
      $('modal').innerHTML = '';
      lastRecalled = name;
      await refreshSnapshots();
    }));
  };
}

$('snapSave').onclick = () => {
  const name = ($('snapName') as HTMLInputElement).value.trim();
  if (!name) return alert('Enter a snapshot name.');
  if (names.includes(name) && !confirm(`Overwrite snapshot "${name}"?`)) return;
  report(engine.request('snapshot.save', { name }).then(() => { lastRecalled = name; return refreshSnapshots(); }));
};
$('snapList').onclick = (e) => {
  const d = (e.target as HTMLElement).dataset;
  if (d.recall) report(recall(d.recall));
  if (d.delete && confirm(`Delete snapshot "${d.delete}"?`)) report(engine.request('snapshot.delete', { name: d.delete }).then(refreshSnapshots));
  if (d.export) report(engine.request<string>('snapshot.export', { name: d.export }).then((json) => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([json], { type: 'application/json' }));
    a.download = `${d.export}.json`;
    a.click();
  }));
};
$('snapAB').onclick = () => {
  const a = ($('snapA') as HTMLSelectElement).value, b = ($('snapB') as HTMLSelectElement).value;
  if (!a || !b) return;
  report(recall(lastRecalled === a ? b : a));
};
$('snapImport').onclick = () => $('snapFile').click();
$('snapFile').onchange = async (e) => {
  const f = (e.target as HTMLInputElement).files?.[0];
  if (!f) return;
  const name = prompt('Import as snapshot named', f.name.replace(/\.json$/i, ''));
  if (name) await report(engine.request('snapshot.import', { name, json: await f.text() }).then(refreshSnapshots));
  (e.target as HTMLInputElement).value = '';
};

// ---------------------------------------------------------------- wiring
engine.on('state', renderStrips);
channels.onChange(() => engine.state && renderStrips(engine.state));
engine.on('status', (st) => { renderMeters(st); renderBanner(st); });
engine.on('open', () => void refreshSnapshots().catch(() => undefined));

// ---------------------------------------------------------------- SPL readout (same weighting/speed as Measurement)
engine.on('status', (st) => {
  const s = st.spl;
  if (!s) return;
  let w = 1, t: 'fast' | 'slow' = 'slow';
  try { w = Number(localStorage.getItem('vt.splW') ?? '1'); t = (localStorage.getItem('vt.splT') as 'fast' | 'slow') ?? 'slow'; } catch { /* defaults */ }
  const letter = ['Z', 'A', 'C'][w] ?? 'A';
  const v = s[t]?.[w] ?? -200;
  $('splLive').innerHTML = v > -150
    ? `L${letter}${t === 'fast' ? 'F' : 'S'} ${v.toFixed(1)} <span class="dim" style="font-size:11px">${s.calibrated ? `dB · L${letter}eq ${s.leq[w].toFixed(1)}` : 'dBFS (uncal.)'}</span>`
    : '';
});
