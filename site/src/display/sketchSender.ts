// Sketch on a USB display: what goes to the device and when. The drawing (sketchModel.ts) is the truth: strokes
// go out as the rectangle changed since the last send in partial mode, the whole drawing in full mode after a
// (re)connect, after `fullAfter` partial sends, on request, and whenever the device says it lost the frame it kept.
import { packRows } from '../protocol/album.ts';
import type { CborMap } from '../protocol/cbor.ts';
import { DisplayError, displayAnswer, type DisplayHello, type DisplayMode } from './client.ts';
import { packedPng } from './png.ts';
import { SketchModel, sketchInk, type SketchInk } from './sketchModel.ts';

/** The part of DisplayClient the sketch uses. */
export interface SketchShow { show(png: Uint8Array, hello: DisplayHello, options?: { mode?: DisplayMode; x?: number; y?: number }): Promise<CborMap> }
export interface SketchStatus { state: 'idle' | 'sending' | 'ok' | 'error'; text: string }
export interface SketchTiming { liftMs: number; everyMs: number; busyMs: number }
export const SKETCH_TIMING: SketchTiming = { liftMs: 150, everyMs: 700, busyMs: 5000 };
export const FULL_AFTER = 30;
// A full refresh nobody asked for: the device did not know the frame it kept, so a rectangle now has white around it.
const LOST = ['first', 'refused', 'cut', 'failed', 'memory', 'depth', 'board'];
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

export class SketchSender {
  /** Partial sends before an automatic full refresh; 0: never. */
  fullAfter = FULL_AFTER;
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
  private strokeAt = 0;
  private sentAt = 0;

  constructor(readonly model: SketchModel, readonly ink: SketchInk, private readonly timing: SketchTiming = SKETCH_TIMING) {}

  /** A device (again): it shows something else, so the whole drawing goes out once. null: no device. */
  attach(client: SketchShow | null, hello: DisplayHello | null = null): void {
    this.client = client; this.hello = hello; this.link++;
    this.whole = true; this.partials = 0; this.drawing = false;
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
    const full = this.asked || (this.ink.partial && (this.whole || (this.fullAfter > 0 && this.partials >= this.fullAfter)));
    if (!full && !this.ink.partial) return;
    const dirty = m.takeDirty();
    if (!full && !dirty) return;
    const rect = full ? m.whole : dirty!;
    const all = rect.w === m.w && rect.h === m.h;
    this.asked = false;
    this.sending = true;
    this.sentAt = Date.now();
    const rows = packRows(m.values(rect), rect.w, rect.h, this.ink.depth); // the pixels as they are now, before any wait
    const what = full ? `a full refresh of the whole drawing (${m.w} x ${m.h})` : `a partial update of ${rect.w} x ${rect.h} pixels at ${rect.x}, ${rect.y}`;
    this.say('sending', `Asked for ${what}; waiting for the panel...`);
    try {
      const png = await packedPng(rows, rect.w, rect.h, this.ink.depth, this.ink.color);
      const options = full ? { mode: 'full' as const } : all ? { mode: 'partial' as const } : { x: rect.x, y: rect.y, mode: 'partial' as const };
      const r = await this.showOnce(client, hello, png, options, what);
      if (link !== this.link) return;
      const lost = !full && r.mode === 'full' && LOST.includes(String(r.why));
      if (full || all) this.whole = false;
      if (lost && !all) this.whole = true;
      if (r.mode === 'full') this.partials = 0;
      else if (r.mode !== 'none') this.partials++;
      this.say('ok', `Asked for ${what}. ${displayAnswer(r)} ${Date.now() - this.sentAt} ms in all.${lost && !all ? ' The device had lost its picture: sending the whole drawing.' : ''}`);
    } catch (e) {
      if (link !== this.link) return;
      if (dirty) m.markDirty(dirty);
      if (full) this.asked = true; // not sent: the next send is the whole drawing again
      this.whole = true; // a refused or cut show makes the device forget the frame it kept
      this.say('error', `Asked for ${what}. Not shown: ${(e as Error).message}. ${this.client === client ? 'Draw on or press Full refresh to send the whole drawing again.' : ''}`.trim());
      return;
    } finally {
      this.sending = false;
      if (link !== this.link && this.client && this.ink.partial) this.schedule(Date.now(), true);
    }
    const again = this.whole || m.hasDirty || this.asked || (this.fullAfter > 0 && this.partials >= this.fullAfter);
    if (again && this.ink.partial) this.schedule(this.drawing && !this.whole ? this.sentAt + this.timing.everyMs : Date.now(), false);
  }
  /** BUSY: wait as long as the device says (bounded) and try once more. */
  private async showOnce(client: SketchShow, hello: DisplayHello, png: Uint8Array, options: { mode?: DisplayMode; x?: number; y?: number }, what: string): Promise<CborMap> {
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
