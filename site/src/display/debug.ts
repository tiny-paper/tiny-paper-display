// The Debug section of the Sketch page, as pure functions: a pasted configuration, the per-key results, the test
// patterns and the report a person copies into a chat. The page (sketch-page.js) wires them to its elements.
import type { DisplayHello, DisplayMode } from './client.ts';
import { sketchBrushes, type Rect } from './sketchModel.ts';
import { FULL_AFTER } from './sketchSender.ts';

/** The page side settings of the Debug section; `depth` null: the deepest partial depth of the device. */
export interface DebugSketch { depth: number | null; mode: DisplayMode | 'auto'; full_after: number; erase: 'auto' | 'partial' | 'full' }
export const DEBUG_DEFAULTS: DebugSketch = { depth: null, mode: 'auto', full_after: FULL_AFTER, erase: 'auto' };
export const TEST_NAMES = ['grays', 'grays_full', 'lines', 'box'] as const;
export type TestName = typeof TEST_NAMES[number];

export type ParsedDebug =
  | { ok: false; error: string }
  | { ok: true; tuning: Record<string, unknown> | 'defaults' | undefined; sketch: Record<string, unknown>; extra: string[] };

/** The text as JSON: `{ "tuning": {...} | "defaults", "sketch": {...} }`; an error says where (line and column). */
export function parseDebug(text: string): ParsedDebug {
  let v: unknown;
  try { v = JSON.parse(text); } catch (e) {
    const msg = String((e as Error).message);
    const pos = /position (\d+)/.exec(msg);
    let where = '';
    if (pos) {
      const before = text.slice(0, Number(pos[1]));
      where = ` (line ${before.split('\n').length}, column ${before.length - before.lastIndexOf('\n')})`;
    }
    return { ok: false, error: `Not valid JSON: ${msg.replace(/ in JSON at position \d+.*$/, '').replace(/\s*\(line \d+ column \d+\)$/, '')}${where}. Nothing was sent.` };
  }
  if (typeof v !== 'object' || v === null || Array.isArray(v)) return { ok: false, error: 'The text must be a JSON object like { "tuning": { ... }, "sketch": { ... } }. Nothing was sent.' };
  const o = v as Record<string, unknown>;
  const extra = Object.keys(o).filter((k) => k !== 'tuning' && k !== 'sketch');
  const t = o.tuning;
  if (t !== undefined && t !== 'defaults' && (typeof t !== 'object' || t === null || Array.isArray(t))) return { ok: false, error: '"tuning" must be an object of keys, or "defaults". Nothing was sent.' };
  if (o.sketch !== undefined && (typeof o.sketch !== 'object' || o.sketch === null || Array.isArray(o.sketch))) return { ok: false, error: '"sketch" must be an object. Nothing was sent.' };
  return { ok: true, tuning: t as Record<string, unknown> | 'defaults' | undefined, sketch: (o.sketch ?? {}) as Record<string, unknown>, extra };
}

export interface KeyResult { key: string; applied: boolean; text: string }

/**
 * The page side keys against what the device says it does. Returns the settings that result and one line per key.
 * `current` is what is in effect; a refused key leaves its setting as it was. Keys are taken in the order depth, mode (the
 * others after), so a mode that needs 1 bit is judged against the depth of the same configuration.
 */
export function checkSketch(sketch: Record<string, unknown>, hello: DisplayHello, current: DebugSketch): { next: DebugSketch; results: KeyResult[] } {
  const next = { ...current };
  const results: KeyResult[] = [];
  const known = ['depth', 'mode', 'full_after', 'erase', 'test'];
  const refuse = (key: string, why: string): void => { results.push({ key: `sketch.${key}`, applied: false, text: `refused: ${why}` }); };
  const ok = (key: string, text: string): void => { results.push({ key: `sketch.${key}`, applied: true, text: `applied${text ? `: ${text}` : ''}` }); };
  for (const key of Object.keys(sketch)) if (!known.includes(key)) refuse(key, `no such key; known: ${known.join(' ')}`);
  const has = (k: string): boolean => Object.hasOwn(sketch, k);
  if (has('depth')) {
    const d = sketch.depth;
    if (d === null || d === 'auto') { next.depth = null; ok('depth', 'the deepest depth the device updates partially at'); }
    else if (!hello.partial_depths.length) refuse('depth', 'this device has no partial updates');
    else if (typeof d !== 'number' || !hello.partial_depths.includes(d)) refuse('depth', `the device does partial updates at ${hello.partial_depths.join(', ')} bit only`);
    else { next.depth = d; ok('depth', `${d} bit`); }
  }
  const depthNow = next.depth ?? (hello.partial_depths.length ? Math.max(...hello.partial_depths) : 0);
  if (has('mode')) {
    const m = sketch.mode;
    const modes = hello.modes.filter((x) => x !== 'none');
    if (m === 'auto' || m === 'partial') { next.mode = m; ok('mode', String(m)); }
    else if (typeof m !== 'string' || !modes.includes(m)) refuse('mode', `the device lists ${modes.join(', ')}; or auto`);
    else if (!['full', 'partial'].includes(m) && depthNow !== 1) refuse('mode', `${m} needs 1 bit and strokes go at ${depthNow} bit (set sketch.depth 1)`);
    else { next.mode = m as DisplayMode; ok('mode', m); }
  }
  if (has('full_after')) {
    const n = sketch.full_after;
    if (typeof n !== 'number' || !Number.isInteger(n) || n < 0 || n > 250) refuse('full_after', 'a whole number from 0 (never) to 250');
    else { next.full_after = n; ok('full_after', String(n)); }
  }
  if (has('erase')) {
    const e = sketch.erase;
    if (e !== 'auto' && e !== 'partial' && e !== 'full') refuse('erase', 'auto, partial or full');
    else { next.erase = e; ok('erase', e); }
  }
  if (has('test') && !(sketch.test === null || (typeof sketch.test === 'string' && (TEST_NAMES as readonly string[]).includes(sketch.test)))) refuse('test', `${TEST_NAMES.join(', ')}`);
  return { next, results };
}

/** What `test` names, or null: ok to run after the other keys are in effect. */
export function testOf(sketch: Record<string, unknown>): TestName | null {
  return typeof sketch.test === 'string' && (TEST_NAMES as readonly string[]).includes(sketch.test) ? sketch.test as TestName : null;
}

/** The device part: one `set` request for these keys; values the device cannot read (nested) are refused here, in words. */
export function tuningRequest(tuning: Record<string, unknown> | 'defaults'): { req: Record<string, unknown> | 'defaults'; results: KeyResult[] } {
  if (tuning === 'defaults') return { req: 'defaults', results: [] };
  const req: Record<string, unknown> = {};
  const results: KeyResult[] = [];
  for (const [k, v] of Object.entries(tuning)) {
    if (typeof v === 'number' && Number.isInteger(v) || typeof v === 'string' || typeof v === 'boolean') req[k] = v;
    else results.push({ key: `tuning.${k}`, applied: false, text: 'refused: the value is a whole number or a word' });
  }
  return { req, results };
}

/** One line per tuning key sent: applied, or refused with the device's words. */
export function tuningResults(sent: string[], refused: Record<string, unknown> | undefined, tuning: Record<string, unknown> | undefined): KeyResult[] {
  return sent.map((k) => {
    const why = refused?.[k];
    return why !== undefined ? { key: `tuning.${k}`, applied: false, text: `refused: ${String(why)}` }
      : { key: `tuning.${k}`, applied: true, text: `applied${tuning && k in tuning ? `: ${String(tuning[k])}` : ''}` };
  });
}

export type TestStep = { rect: Rect; value: number } | { line: [number, number, number, number, number, number] };

/**
 * The test patterns, the same on every try for a panel size, so results are comparable. Each step is sent by itself as
 * one partial update (after a full refresh of the blank panel). `paper` is the white level.
 *   grays: one bar for each level from black to white (the white bar changes nothing, so it is not sent)
 *   grays_full: the same bars, all drawn first and shown with one full refresh
 *   lines: thin, medium and thick black lines, horizontal and then vertical
 *   box: a filled mid gray box, then a white box inside it
 */
export function testPlan(name: TestName, w: number, h: number, paper: number): TestStep[] {
  const X = (f: number): number => Math.round(w * f), Y = (f: number): number => Math.round(h * f);
  if (name === 'grays' || name === 'grays_full') {
    const n = paper + 1, bw = Math.floor(w * 0.8 / n);
    return Array.from({ length: n }, (_, i) => ({ rect: { x: X(0.1) + i * bw, y: Y(0.25), w: bw, h: Y(0.2) }, value: i }));
  }
  if (name === 'lines') {
    const b = sketchBrushes(w);
    return [
      ...b.map((s, i): TestStep => ({ line: [X(0.1), Y(0.55 + 0.05 * i), X(0.9), Y(0.55 + 0.05 * i), s, 0] })),
      ...b.map((s, i): TestStep => ({ line: [X(0.2 + 0.3 * i), Y(0.72), X(0.2 + 0.3 * i), Y(0.92), s, 0] })),
    ];
  }
  const mid = Math.round(paper / 2);
  return [
    { rect: { x: X(0.3), y: Y(0.5), w: X(0.4), h: Y(0.3) }, value: mid },
    { rect: { x: X(0.4), y: Y(0.58), w: X(0.2), h: Y(0.14) }, value: paper },
  ];
}

export interface HistoryLine { at: string; text: string; lines?: string[] }
export const REPORT_UPDATES = 12;
export const REPORT_LOG_LINES = 8;

/** At most `max` of the lines, the `panel` ones first (they hold the breakdown), in their order; how many were cut. */
export function pickLines(lines: readonly string[], max = REPORT_LOG_LINES): { kept: string[]; cut: number } {
  const idx = lines.map((l, i) => [l, i] as const);
  const chosen = [...idx.filter(([l]) => /^(W |I )?panel\b/.test(l)), ...idx.filter(([l]) => !/^(W |I )?panel\b/.test(l))].slice(0, max).map(([, i]) => i).sort((a, b) => a - b);
  return { kept: chosen.map((i) => lines[i]!), cut: lines.length - chosen.length };
}

/** The device log lines of one update as the reports show them: indented, the `panel` ones first, a note when some are cut. */
export function indentedLog(all: readonly string[]): string[] {
  const { kept, cut } = pickLines(all);
  const out = kept.map((k) => `    | ${k}`);
  if (cut) out.push(`    | (${cut} more device log line${cut === 1 ? '' : 's'} cut; at most ${REPORT_LOG_LINES} are kept)`);
  return out;
}
/** The tuning line of a report. */
export function tuningLine(h: DisplayHello): string {
  return h.tuning ? `tuning: ${Object.entries(h.tuning).map(([k, v]) => `${k}=${v}`).join(' ')}` : 'tuning: none (this device has no panel tuning)';
}

/** The report: compact text to copy into a chat (device, tuning, page settings, what was seen, the last status lines). */
export function reportText(o: { hello: DisplayHello | null; sketch: DebugSketch; depthUsed: number | null; saw: string; history: readonly HistoryLine[]; test?: string | null; withLog?: boolean; logSeen?: boolean }): string {
  const h = o.hello;
  const lines = ['tiny-paper display report'];
  if (!h) return `${lines[0]}\nno device connected`;
  lines.push(`device: ${h.board} ${h.w}x${h.h}, display firmware ${h.fw}`);
  lines.push(`device partial depths: ${h.partial_depths.length ? h.partial_depths.join(',') : 'none'}; modes: ${h.modes.join(',')}`);
  lines.push(tuningLine(h));
  lines.push(`sketch: depth=${o.sketch.depth ?? 'auto'}${o.depthUsed ? ` (${o.depthUsed} bit in use)` : ''} mode=${o.sketch.mode} full_after=${o.sketch.full_after} erase=${o.sketch.erase}${o.test ? ` test=${o.test}` : ''}`);
  lines.push(`what I saw: ${o.saw.trim() || '(nothing written)'}`);
  const note = o.withLog === false ? '' : !o.logSeen && o.history.length ? ' (this firmware wrote no device log lines)' : ' with the device log lines of each update, indented';
  lines.push(`last ${Math.min(REPORT_UPDATES, o.history.length)} status lines${note}:`);
  for (const l of o.history.slice(-REPORT_UPDATES)) {
    lines.push(`${l.at} ${l.text}`);
    if (o.withLog === false) continue;
    lines.push(...indentedLog(l.lines ?? []));
  }
  return lines.join('\n');
}
