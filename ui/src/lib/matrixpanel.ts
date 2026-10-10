// Measurement matrix (M1-style workflow): measurement groups × mic positions. One mic is
// enough: place it, press ▶ for that position, the engine plays each group alone, captures,
// checks quality and labels the result "<group> @ <position>". Move the mic, repeat.
// With several mics, give positions their own mic and press "▶ All": every position is
// captured at once from the same burst of pink noise.
import { esc } from './statusbar';
import { spatialAverage } from './average';
import type { Engine } from './engine';
import type { StoredTrace } from './tunepanels';
import type { SystemState, Trace } from './types';

export interface MatrixCtx {
  engine: Engine;
  traces: () => StoredTrace[];
  putTrace: (t: Trace, extra: Partial<StoredTrace>) => void; // add, or replace one with the same name
  showOnly: (names: string[]) => void;
  report: (p: Promise<unknown>) => void;
}

export function mountMatrix(el: HTMLElement, ctx: MatrixCtx): void {
  const { engine } = ctx;
  const rejected = new Map<string, string>(); // "g:p" -> reason
  const running = new Set<string>();           // "g:p" being measured
  el.innerHTML = `
    <h2>Measurement matrix</h2>
    <div class="dim">One mic is enough: put it at a position and press ▶. Each group plays alone (generator level from the Measure tab), is captured, quality-checked and labelled, then move the mic. With several mics, give each position its own mic and press ▶ All to capture every position at once.</div>
    <h2>Mic positions</h2>
    <div id="mxPos"></div>
    <div class="row"><input id="mxPosName" type="text" class="grow" placeholder="e.g. FOH, Front, Balcony"/><button id="mxPosAdd">+ Position</button></div>
    <h2>Groups (played alone)</h2>
    <div id="mxGroups"></div>
    <div class="row"><button id="mxGroupAdd">+ Group</button><button id="mxPerOutput">One per output</button></div>
    <h2>Captures</h2>
    <div class="scroll"><table id="mxGrid"></table></div>
    <div class="row"><button id="mxAll" class="grow" title="measure every position that has its own mic, at the same time">▶ All positions with a mic</button><button id="mxCancel">Cancel</button></div>
    <div id="mxMsg" class="dim"></div>`;
  const $ = <T extends HTMLElement>(id: string) => el.querySelector<T>(`#${id}`)!;
  const patch = (path: string, value: unknown) => ctx.report(engine.patch(path, value));

  const traceFor = (s: SystemState, g: number, p: number) =>
    ctx.traces().find((t) => t.name === `${s.measure.groups[g].name} @ ${s.measure.positions[p]}`);

  function render(): void {
    const s = engine.state;
    if (!s) return;
    if (el.contains(document.activeElement) && (document.activeElement as HTMLElement).matches('input[type=text]')) return;
    const m = s.measure;

    const mics = s.sources.map((src, i) => ({ src, i })).filter(({ src }) => src.type === 'mic');
    const micOpts = (pi: number) => {
      const cur = m.positionMics?.[pi] ?? -1;
      return `<option value="-1"${cur < 0 ? ' selected' : ''}>main mic (move it)</option>` + mics.map(({ src, i }) => {
        const usedAt = (m.positionMics ?? []).findIndex((x, k) => x === i && k !== pi);
        return `<option value="${i}"${cur === i ? ' selected' : ''}${usedAt >= 0 ? ' disabled' : ''}>${esc(src.name)}${usedAt >= 0 ? ` (at ${esc(m.positions[usedAt])})` : ''}</option>`;
      }).join('');
    };
    $('mxPos').innerHTML = m.positions.map((p, i) => `<div class="row">
      <input type="text" data-pos="${i}" value="${esc(p)}" class="grow"/><select data-posmic="${i}" title="mic standing at this position">${micOpts(i)}</select>
      <button data-delpos="${i}">×</button></div>`).join('') || '<div class="dim">No positions yet.</div>';
    ($('mxAll') as HTMLButtonElement).disabled = !(m.positionMics ?? []).some((x) => x >= 0);

    $('mxGroups').innerHTML = m.groups.map((g, gi) => `<div class="row" style="flex-wrap:wrap">
      <input type="text" data-gname="${gi}" value="${esc(g.name)}" style="width:110px"/>
      ${s.outputs.map((o, oi) => `<button data-gout="${gi}:${oi}" class="${g.outputs.includes(oi) ? 'on' : ''}" title="${esc(o.name)}">${oi + 1}</button>`).join('')}
      <button data-delgroup="${gi}">×</button></div>`).join('') || '<div class="dim">No groups yet.</div>';

    if (!m.groups.length || !m.positions.length) { $('mxGrid').innerHTML = '<tr><td class="dim">Add positions and groups.</td></tr>'; return; }
    $('mxGrid').innerHTML = `<tr><th></th>${m.positions.map((p, pi) =>
        `<th>${esc(p)}<br><button data-runpos="${pi}" title="measure every group at this position">▶</button></th>`).join('')}<th></th></tr>` +
      m.groups.map((g, gi) => `<tr><th>${esc(g.name)}</th>${m.positions.map((_, pi) => {
        const key = `${gi}:${pi}`, t = traceFor(s, gi, pi);
        const txt = running.has(key) ? '<span class="warn">…</span>'
          : t ? `<span class="ok">✓ ${Math.round((t.quality ?? 1) * 100)}%</span>`
          : rejected.has(key) ? `<span class="bad" title="${esc(rejected.get(key)!)}">✗</span>` : '<span class="dim">—</span>';
        return `<td data-cell="${key}" style="cursor:pointer;text-align:center" title="click: measure this group here">${txt}</td>`;
      }).join('')}<td><button data-avg="${gi}" title="average this group over all positions, for Auto-EQ">avg</button></td></tr>`).join('');
  }

  el.addEventListener('change', (e) => {
    const t = e.target as HTMLInputElement;
    const s = engine.state;
    if (!s) return;
    if (t.dataset.pos) { const p = [...s.measure.positions]; p[+t.dataset.pos] = t.value.trim() || p[+t.dataset.pos]; patch('measure/positions', p); }
    if (t.dataset.gname) patch(`measure/groups/${t.dataset.gname}/name`, t.value.trim() || `Group ${+t.dataset.gname + 1}`);
    if (t.dataset.posmic) {
      const pm = s.measure.positions.map((_, i) => s.measure.positionMics?.[i] ?? -1);
      pm[+t.dataset.posmic] = Number(t.value);
      patch('measure/positionMics', pm);
    }
  });
  el.addEventListener('focusout', () => setTimeout(render, 0));
  el.addEventListener('click', (e) => {
    const d = (e.target as HTMLElement).closest<HTMLElement>('[data-cell],button')?.dataset ?? {};
    const s = engine.state;
    if (!s) return;
    const m = s.measure;
    if (d.delpos) {
      const keep = (_: unknown, i: number) => i !== +d.delpos!;
      ctx.report(engine.patchMany([
        { path: 'measure/positionMics', value: m.positions.map((_, i) => m.positionMics?.[i] ?? -1).filter(keep) },
        { path: 'measure/positions', value: m.positions.filter(keep) },
      ]));
    }
    if (d.delgroup) patch('measure/groups', m.groups.filter((_, i) => i !== +d.delgroup!));
    if (d.gout) {
      const [gi, oi] = d.gout.split(':').map(Number);
      const outs = m.groups[gi].outputs;
      patch(`measure/groups/${gi}/outputs`, outs.includes(oi) ? outs.filter((x) => x !== oi) : [...outs, oi].sort((a, b) => a - b));
    }
    if (d.runpos) run(+d.runpos);
    if (d.cell) { const [gi, pi] = d.cell.split(':').map(Number); run(pi, [gi]); }
    if (d.avg) average(+d.avg);
  });
  $('mxPosAdd').onclick = () => {
    const s = engine.state, name = ($('mxPosName') as HTMLInputElement).value.trim();
    if (!s) return;
    patch('measure/positions', [...s.measure.positions, name || `Pos ${s.measure.positions.length + 1}`]);
    ($('mxPosName') as HTMLInputElement).value = '';
  };
  $('mxGroupAdd').onclick = () => {
    const s = engine.state;
    if (s) patch('measure/groups', [...s.measure.groups, { name: `Group ${s.measure.groups.length + 1}`, outputs: [] }]);
  };
  $('mxPerOutput').onclick = () => {
    const s = engine.state;
    if (s && (!s.measure.groups.length || confirm('Replace the groups with one group per output?')))
      patch('measure/groups', s.outputs.map((o, i) => ({ name: o.name, outputs: [i] })));
  };
  $('mxCancel').onclick = () => ctx.report(engine.request('matrix.cancel'));
  $('mxAll').onclick = () => ctx.report(engine.request('matrix.run', { all: true }));

  function run(position: number, groups?: number[]): void {
    ctx.report(engine.request('matrix.run', groups ? { position, groups } : { position }));
  }

  // Power average of one group over every position measured, shown alone (ready for Auto-EQ).
  function average(gi: number): void {
    const s = engine.state;
    if (!s) return;
    const ts = s.measure.positions.map((_, pi) => traceFor(s, gi, pi)).filter((t): t is StoredTrace => !!t);
    if (!ts.length) return alert('Measure this group at one or more positions first.');
    const avg: StoredTrace = {
      ...spatialAverage(ts, `${s.measure.groups[gi].name} AVG (${ts.length})`)!,
      position: undefined, quality: undefined, // an average belongs to no single position
    };
    ctx.putTrace(avg, { group: s.measure.groups[gi].name });
    ctx.showOnly([avg.name]);
    $('mxMsg').textContent = `${avg.name} created and shown alone: open Auto-EQ to fit it.`;
  }

  engine.on('matrix', (ev) => {
    const s = engine.state;
    const gName = s && ev.group !== undefined ? s.measure.groups[ev.group]?.name : '';
    const pName = s && ev.position !== undefined ? s.measure.positions[ev.position] : '';
    const key = `${ev.group}:${ev.position}`;
    if (ev.stage === 'measuring') { running.add(key); $('mxMsg').textContent = `measuring ${gName}…`; }
    if (ev.stage === 'retry') $('mxMsg').textContent = `${gName}: disturbed (quality ${Math.round((ev.quality ?? 0) * 100)}%), measuring again…`;
    if (ev.stage === 'captured' || ev.stage === 'rejected') running.delete(key);
    if (ev.stage === 'captured' && ev.trace) {
      rejected.delete(key);
      ctx.putTrace(ev.trace, { group: gName, position: pName, quality: ev.quality });
    }
    if (ev.stage === 'rejected') { rejected.set(key, ev.message ?? 'rejected'); $('mxMsg').innerHTML = `<span class="bad">${esc(gName)} @ ${esc(pName)}: ${esc(ev.message ?? '')}</span>`; }
    if (ev.stage === 'done' || ev.stage === 'error') { running.clear(); $('mxMsg').textContent = ev.stage === 'done' ? 'Done.' : (ev.message ?? ''); }
    render();
  });
  engine.on('state', render);
  window.addEventListener('vt-traces', render);
  render();
}
