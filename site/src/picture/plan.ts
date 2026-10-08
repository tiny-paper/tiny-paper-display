// What the Picture page offers for a device and what it says about a send: the depths, the refresh choices with the
// reason one is unavailable, the Compare sequence, the report. Pure functions of the device's hello.
import type { DisplayHello, DisplayMode } from '../display/client.ts';
import type { Dither, Settings } from './prepare.ts';

export const BOARD_TITLES: Record<string, string> = { trmnl_x: 'TRMNL X', trmnl_og: 'TRMNL OG', picpak: 'PicPak' };
export const DITHER_NAMES: Record<Dither, string> = { none: 'nearest level', floyd: 'Floyd-Steinberg', ordered: 'ordered 4x4' };

/** What the device's hello says about partial updates, in words: "Partial updates at: 1, 2 and 4 bit (16 grays)". */
export function partialLine(hello: DisplayHello): string {
  const d = [...hello.partial_depths].sort((a, b) => a - b);
  if (!d.length) return 'Partial updates at: none (full refreshes only)';
  const list = d.length === 1 ? `${d[0]}` : `${d.slice(0, -1).join(', ')} and ${d.at(-1)}`;
  return `Partial updates at: ${list} bit${d.at(-1)! > 1 ? ` (${2 ** d.at(-1)!} grays)` : ''}`;
}
export function deviceLine(hello: DisplayHello): string {
  return `${BOARD_TITLES[hello.board] ?? hello.board}, ${hello.w} x ${hello.h}, display firmware ${hello.fw}.`;
}

export interface DepthChoice { key: string; depth: number; color: boolean; label: string }
/** Each depth the device takes, and on the PicPak its four colours. */
export function depthChoices(hello: DisplayHello): DepthChoice[] {
  const out: DepthChoice[] = [...hello.depths].sort((a, b) => a - b).map((d) => ({
    key: String(d), depth: d, color: false, label: d === 1 ? '1 bit (black and white)' : `${d} bit (${2 ** d} grays)` }));
  if (hello.colors) out.push({ key: 'color', depth: 2, color: true, label: '4 colours (black, white, red, yellow)' });
  return out;
}
/** The deepest choice: the colours on the PicPak, else the deepest gray. */
export const deepest = (hello: DisplayHello): DepthChoice => depthChoices(hello).at(-1)!;

export type Refresh = 'auto' | 'partial' | 'full';
export interface RefreshChoice { key: Refresh; label: string; enabled: boolean; why: string }
/** Auto, partial, full for this depth. Partial is off where the device cannot do it at that depth, and says why. */
export function refreshChoices(hello: DisplayHello, depth: DepthChoice): RefreshChoice[] {
  let why = '';
  if (!hello.modes.includes('partial') || !hello.partial_depths.length) why = 'this device has no partial refresh';
  else if (depth.color || !hello.partial_depths.includes(depth.depth)) why = `this device does partial updates at ${[...hello.partial_depths].sort((a, b) => a - b).join(', ')} bit only, not at ${depth.color ? 'colour' : `${depth.depth} bit`}`;
  return [
    { key: 'auto', label: 'Auto (the device decides)', enabled: true, why: '' },
    { key: 'partial', label: 'Partial (only what changed)', enabled: !why, why },
    { key: 'full', label: 'Full (flashes the whole panel)', enabled: true, why: '' },
  ];
}
export const refreshOptions = (r: Refresh): { mode?: DisplayMode } => (r === 'auto' ? {} : { mode: r });

/** Short words for the settings of a send, for the status line and the report. */
export function describeSettings(s: Settings, rotation: number, refresh: Refresh): string {
  const depth = s.color ? '4 colours' : s.depth === 1 ? '1 bit' : `${s.depth} bit gray`;
  const dither = s.dither === 'none' && !s.color && s.depth === 1 ? `nearest level (threshold ${s.threshold})` : DITHER_NAMES[s.dither];
  const fit = { fit: 'whole picture', fill: 'fill and crop', stretch: 'stretched' }[s.fit];
  return `${depth}, ${dither}, ${fit}, border ${s.border}, rotated ${rotation}${s.rotate === 'auto' ? ' (auto)' : ''}, ${refresh} refresh`;
}

export interface CompareStep { label: string; choice: DepthChoice; dither: Dither }
/** 1 bit nearest, 1 bit Floyd-Steinberg, the deepest nearest, the deepest Floyd-Steinberg (the deepest ones only if deeper than 1 bit). */
export function comparePlan(hello: DisplayHello): CompareStep[] {
  const one = depthChoices(hello).find((c) => c.depth === 1 && !c.color);
  const top = deepest(hello);
  const steps: CompareStep[] = [];
  const add = (choice: DepthChoice, dither: Dither): void => {
    steps.push({ label: `${choice.color ? '4 colours' : `${choice.depth} bit`} ${DITHER_NAMES[dither]}`, choice, dither });
  };
  if (one) { add(one, 'none'); add(one, 'floyd'); }
  if (top !== one) { add(top, 'none'); add(top, 'floyd'); }
  return steps;
}

export interface SendRecord { label: string; settings: string; ok: boolean; answer: string; totalMs: number }
/** The text a person copies into a chat. */
export function reportText(r: { hello: DisplayHello | null; picture: string; sends: SendRecord[]; saw: string }): string {
  const lines = ['tiny-paper display, picture report', `Device: ${r.hello ? `${deviceLine(r.hello)} ${partialLine(r.hello)}` : 'none connected'}`, `Picture: ${r.picture || 'none chosen'}`];
  r.sends.forEach((s, i) => lines.push(`${i + 1}. ${s.label ? `${s.label}: ` : ''}${s.settings}. ${s.ok ? '' : 'Not shown: '}${s.answer} Total ${s.totalMs} ms.`));
  if (!r.sends.length) lines.push('Nothing sent yet.');
  lines.push(`What I saw: ${r.saw}`);
  return lines.join('\n');
}
