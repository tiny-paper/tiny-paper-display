// Sketch on a USB display: what goes to the device and when. The drawing (sketchModel.ts) is the truth: strokes
// go out as the rectangle changed since the last send in partial mode, the whole drawing in full mode after a
// (re)connect, after `fullAfter` partial sends, on request, and whenever the device says it lost the frame it kept.
import { packRows } from '../protocol/album.ts';
import type { CborMap } from '../protocol/cbor.ts';
import { DisplayError, displayAnswer, type DisplayHello, type DisplayMode } from './client.ts';
import { packedPng } from './png.ts';
import { SketchModel, sketchInk, type SketchInk } from './sketchModel.ts';
import { toneGamma, toneTable } from './tone.ts';

/** The part of DisplayClient the sketch uses. */
export interface SketchShow { show(png: Uint8Array, hello: DisplayHello, options?: { mode?: DisplayMode; passes?: number; x?: number; y?: number }): Promise<CborMap> }
export interface SketchStatus { state: 'idle' | 'sending' | 'ok' | 'error'; text: string }
export interface SketchTiming { liftMs: number; everyMs: number; busyMs: number }
export const SKETCH_TIMING: SketchTiming = { liftMs: 150, everyMs: 700, busyMs: 5000 };
export const FULL_AFTER = 30;
/** Sends that made pixels lighter by a partial update (which cleans weakly) before a full refresh follows by itself. */
export const LIGHTEN_AFTER = 3;
type Options = { mode?: DisplayMode; passes?: number; x?: number; y?: number };
/**
 * The strongest refresh of a rectangle the device offers for pixels that get lighter, from what its hello says:
 * the modes other than `partial` need 1 bit (usb-display.md "Area and refresh mode"). null: none, partial is all there is.
 */
export function cleanMode(hello: DisplayHello, depth: number): { mode: DisplayMode; passes?: number; words: string } | null {
  if (depth !== 1 || !hello.partial_depths.includes(1)) return null;
  if (hello.modes.includes('inverse')) return { mode: 'inverse', words: 'an inverse update' };
  if (hello.modes.includes('passes')) return { mode: 'passes', passes: 2, words: 'a repeated update (2 passes)' };
  if (hello.modes.includes('partial_all')) return { mode: 'partial_all', words: 'an update of every pixel' };
  return null;
}
// A full refresh nobody asked for: the device did not know the frame it kept, so a rectangle now has white around it.
const LOST = ['first', 'refused', 'cut', 'failed', 'memory', 'depth', 'board'];
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

export class SketchSender {
  /** Partial sends before an automatic full refresh; 0: never. */
  fullAfter = FULL_AFTER;
  /** Tone step (tone.ts, a measuring aid): 0 sends every level as drawn. Gray panels only. */
  tone = 0;
  onStatus: (s: SketchStatus) => void = () => {};
  status: SketchStatus = { state: 'idle', text: 'Nothing sent yet.' };
  private client: SketchShow | null = null;
  private hello: DisplayHello | null = null;
  private link = 0;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private due = 0;
  private sending = false;
  private drawing = false;
  private whole = true;
  private asked = false;
  private partials = 0;
  private lightened = 0; // sends in a row that made pixels lighter by a plain partial update
  private note = ''; // why the next full refresh was asked, said in its status line
  private strokeAt = 0;
  private sentAt = 0;

  constructor(readonly model: SketchModel, readonly ink: SketchInk, private readonly timing: SketchTiming = SKETCH_TIMING) {}

  /** A device (again): it shows something else, so the whole drawing goes out once. null: no device. */
  attach(client: SketchShow | null, hello: DisplayHello | null = null): void {
    this.client = client; this.hello = hello; this.link++;
    this.whole = true; this.partials = 0; this.lightened = 0; this.drawing = false;
    this.cancel();
    if (client && this.ink.partial) this.schedule(Date.now(), true);
    else if (!client) this.say('idle', 'No device connected: the drawing is kept and sent when one connects.');
  }
  /** The pen went down. */
  begin(): void { this.drawing = true; this.strokeAt = Date.now(); }
  /** The drawing changed under the pen: at most one send every `everyMs`. */
  changed(): void { if (this.drawing) this.schedule(Math.max(this.strokeAt, this.sentAt) + this.timing.everyMs, false); }
  /** The pen lifted, or an undo or clear changed the drawing: send shortly. */
  lift(): void { this.drawing = false; this.schedule(Date.now() + this.timing.liftMs, true); }
  /** Clear: the whole drawing goes out in full mode, always (a partial update leaves faint traces of what was removed). */
  cleared(): void {
    if (!this.ink.partial) { this.lift(); return; }
    this.drawing = false; this.note = ' Clear refreshes the whole panel: a partial update would leave faint traces of what was removed.';
    this.fullRefresh();
  }
  /** Send the whole drawing in full mode. */
  fullRefresh(): void { this.asked = true; this.schedule(Date.now(), true); }
  stop(): void { this.cancel(); }

  private cancel(): void { if (this.timer) clearTimeout(this.timer); this.timer = null; }
  private schedule(at: number, replace: boolean): void {
    if (this.timer && !replace && this.due <= at) return;
    this.cancel();
    this.due = at;
    this.timer = setTimeout(() => { this.timer = null; void this.flush(); }, Math.max(0, at - Date.now()));
  }
  private say(state: SketchStatus['state'], text: string): void { this.status = { state, text }; this.onStatus(this.status); }

  private async flush(): Promise<void> {
    const client = this.client, hello = this.hello, link = this.link, m = this.model;
    if (!client || !hello || this.sending) return; // a send in flight looks at the drawing again when it ends
    const wornOut = this.lightened >= LIGHTEN_AFTER;
    const full = this.asked || (this.ink.partial && (this.whole || wornOut || (this.fullAfter > 0 && this.partials >= this.fullAfter)));
    const note = this.asked ? this.note : wornOut && !this.whole ? ` Full refresh by itself: ${this.lightened} sends in a row erased pixels by a partial update, which leaves faint traces.` : '';
    if (!full && !this.ink.partial) return;
    const dirty = m.takeDirty();
    const lighter = m.takeLighter();
    const clean = lighter && !full ? cleanMode(hello, this.ink.depth) : null;
    if (!full && !dirty) return;
    const rect = full ? m.whole : dirty!;
    const all = rect.w === m.w && rect.h === m.h;
    this.asked = false; this.note = '';
    this.sending = true;
    this.sentAt = Date.now();
    const drawn = m.values(rect);
    const lut = !this.ink.color && this.ink.depth > 1 && toneGamma(this.tone) !== 1 ? toneTable(this.ink.paper, this.tone) : null;
    const sent = lut ? drawn.map((v) => lut[v]!) : drawn;
    const rows = packRows(sent, rect.w, rect.h, this.ink.depth); // the pixels as they are now, before any wait
    const remap = lut ? [...new Set(drawn)].filter((v) => v > 0 && v < this.ink.paper && lut[v] !== v).sort((a, b) => a - b).slice(0, 3).map((v) => `gray ${v} sent as level ${lut[v]}`) : [];
    const toneNote = remap.length ? ` Tone: ${remap.join(', ')}.` : '';
    const at = `${rect.w} x ${rect.h} pixels at ${rect.x}, ${rect.y}`;
    const what = full ? `a full refresh of the whole drawing (${m.w} x ${m.h})` : clean ? `${clean.words} of ${at}, to clean the pixels that got lighter` : `a partial update of ${at}`;
    this.say('sending', `Asked for ${what}; waiting for the panel...`);
    try {
      const png = await packedPng(rows, rect.w, rect.h, this.ink.depth, this.ink.color);
      const mode: Options = full ? { mode: 'full' } : clean ? { mode: clean.mode, ...(clean.passes ? { passes: clean.passes } : {}) } : { mode: 'partial' };
      const options: Options = full || all ? mode : { x: rect.x, y: rect.y, ...mode };
      const r = await this.showOnce(client, hello, png, options, what);
      if (link !== this.link) return;
      const lost = !full && r.mode === 'full' && LOST.includes(String(r.why));
      if (full || all) this.whole = false;
      if (lost && !all) this.whole = true;
      if (r.mode === 'full') { this.partials = 0; this.lightened = 0; } else if (r.mode !== 'none') this.partials++;
      const weak = lighter && !clean && r.mode === 'partial';
      if (weak) this.lightened++;
      const trace = weak && this.lightened < LIGHTEN_AFTER ? ` Erasing by a partial update can leave faint traces at ${this.ink.depth} bit: a full refresh follows after ${LIGHTEN_AFTER} such sends (${this.lightened} so far).` : '';
      this.say('ok', `Asked for ${what}. ${displayAnswer(r)} ${Date.now() - this.sentAt} ms in all.${lost && !all ? ' The device had lost its picture: sending the whole drawing.' : ''}${note}${trace}${toneNote}`);
    } catch (e) {
      if (link !== this.link) return;
      if (dirty) m.markDirty(dirty, lighter);
      if (full) { this.asked = true; this.note = note; } // not sent: the next send is the whole drawing again
      this.whole = true; // a refused or cut show makes the device forget the frame it kept
      this.say('error', `Asked for ${what}. Not shown: ${(e as Error).message}. ${this.client === client ? 'Draw on or press Full refresh to send the whole drawing again.' : ''}`.trim());
      return;
    } finally {
      this.sending = false;
      if (link !== this.link && this.client && this.ink.partial) this.schedule(Date.now(), true);
    }
    const again = this.whole || m.hasDirty || this.asked || this.lightened >= LIGHTEN_AFTER || (this.fullAfter > 0 && this.partials >= this.fullAfter);
    if (again && this.ink.partial) this.schedule(this.drawing && !this.whole ? this.sentAt + this.timing.everyMs : Date.now(), false);
  }
  /** BUSY: wait as long as the device says (bounded) and try once more. */
  private async showOnce(client: SketchShow, hello: DisplayHello, png: Uint8Array, options: Options, what: string): Promise<CborMap> {
    try { return await client.show(png, hello, options); } catch (e) {
      if (!(e instanceof DisplayError) || e.codeName !== 'BUSY') throw e;
      const ms = Math.min(this.timing.busyMs, Math.max(100, Number((e.detail as { retry_ms?: unknown } | undefined)?.retry_ms) || 2000));
      this.say('sending', `Asked for ${what}. The panel is busy: trying once more in ${(ms / 1000).toFixed(1)} s...`);
      await sleep(ms);
      return client.show(png, hello, options);
    }
  }
}

/** One drawing per panel shape, kept while the page is open: leaving the page or a reconnect does not lose it. */
let kept: { key: string; sender: SketchSender } | null = null;
export function sketchSession(hello: DisplayHello): SketchSender {
  const ink = sketchInk(hello);
  const key = [hello.w, hello.h, ink.depth, ink.color, ink.partial].join();
  if (kept?.key !== key) kept = { key, sender: new SketchSender(new SketchModel(hello.w, hello.h, ink.paper), ink) };
  return kept.sender;
}
