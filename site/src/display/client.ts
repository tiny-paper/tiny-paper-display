// USB display v1, docs/spec/usb-display.md. Framing and CBOR use the existing link stream.
import type { CborMap } from '../protocol/cbor.ts';

export const DISPLAY_MODES = ['full', 'partial', 'partial_all', 'inverse', 'passes'] as const;
export type DisplayMode = typeof DISPLAY_MODES[number];
export interface DisplayHello {
  proto: number; project: 'display'; fw: string; board: string; w: number; h: number;
  depths: number[]; colors?: string[]; max_payload: number; max_png: number; chunk: number;
  partial_depths: number[]; modes: string[]; full_every: number;
  /** The panel tuning now (usb-display.md "Tuning"), only where the board has knobs. */
  tuning?: Record<string, number | string>;
  /** The picture the device keeps (and its depth is in the hash), absent when it keeps none or does not know it. */
  fingerprint?: string;
  /** The most pixels a partial update takes in its rectangle, per gray depth ("2", "4"); absent: no limit. */
  partial_max?: Record<string, number>;
}
export const DISPLAY_ERRORS: Record<number, string> = {
  1: 'UNKNOWN_CMD', 2: 'BAD_REQUEST', 3: 'BUSY', 10: 'IO', 11: 'SEQUENCE', 13: 'TOO_LARGE',
  14: 'NOMEM', 32: 'NOT_PNG', 33: 'WRONG_SIZE', 34: 'DEPTH', 35: 'BAD_PNG', 36: 'MODE', 37: 'RECT',
};
const refusal: Record<number, string> = {
  1: 'This firmware does not know that command', 2: 'The request is invalid', 3: 'The panel is busy; try again after it finishes',
  10: 'The panel could not finish the update', 11: 'The picture transfer was interrupted; send it again',
  13: 'The picture is too large', 14: 'The device ran out of memory', 32: 'The picture is not a PNG',
  33: 'The picture does not fit the panel', 34: 'The panel cannot show these colors or bit depth',
  35: 'The PNG is damaged', 36: 'The panel cannot run this refresh mode', 37: 'The rectangle is outside the panel or misaligned',
};
export class DisplayError extends Error {
  readonly codeName: string;
  constructor(readonly code: number, readonly detail: unknown, readonly deviceMessage: string) {
    super(`${refusal[code] ?? 'The device refused the request'}${deviceMessage ? `: ${deviceMessage}` : ''}`);
    this.codeName = DISPLAY_ERRORS[code] ?? String(code);
  }
}
export interface DisplayWire { requestRaw(cmd: number, req: CborMap, waitMs?: number): Promise<CborMap> }
export function displayHello(value: unknown): DisplayHello {
  const h = value as DisplayHello;
  if (!h || h.project !== 'display') throw new Error('The connected device does not run the display firmware');
  if (typeof h.board !== 'string' || typeof h.fw !== 'string' || h.proto !== 1 || !Number.isInteger(h.w) || !Number.isInteger(h.h) || h.w < 1 || h.h < 1 || h.w * h.h > 4_194_304 ||
      !Number.isInteger(h.max_png) || h.max_png < 57 || h.max_png > 1_319_644 || h.max_payload !== 6144 || h.chunk !== 4096 ||
      !Array.isArray(h.depths) || !h.depths.length || h.depths.some((d) => ![1, 2, 4].includes(d)) ||
      !Array.isArray(h.modes) || h.modes.some((m) => !(DISPLAY_MODES as readonly string[]).includes(m)) || !h.modes.includes('full') ||
      !Array.isArray(h.partial_depths) || h.partial_depths.some((d) => !h.depths.includes(d)) ||
      !Number.isInteger(h.full_every) || h.full_every < 0 || h.full_every > 250 ||
      (h.colors !== undefined && (!Array.isArray(h.colors) || h.colors.join(',') !== 'black,white,red,yellow'))) throw new Error('The display firmware sent invalid panel limits');
  return h;
}
export class DisplayClient {
  private queue: Promise<unknown> = Promise.resolve();
  constructor(private readonly wire: DisplayWire) {}
  async request(cmd: number, req: CborMap): Promise<CborMap> {
    const r = await this.wire.requestRaw(cmd, req);
    if (typeof r.err === 'number') throw new DisplayError(r.err, r.detail, String(r.msg ?? ''));
    return r;
  }
  async hello(): Promise<DisplayHello> { return displayHello(await this.request(1, { proto: 1, app: 'tiny-paper-webapp' })); }
  private exclusive<T>(run: () => Promise<T>): Promise<T> {
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => undefined);
    return p;
  }
  show(png: Uint8Array, hello: DisplayHello, options: { mode?: DisplayMode; passes?: number; x?: number; y?: number } = {}): Promise<CborMap> {
    return this.exclusive(async () => {
      displayHello(hello);
      if (png.length < 57 || png.length > hello.max_png) throw new Error('The PNG is outside the device size limit');
      if (options.mode && !hello.modes.includes(options.mode)) throw new Error('The panel cannot run this refresh mode');
      if ((options.mode === 'passes') !== (options.passes !== undefined) ||
          (options.passes !== undefined && (!Number.isInteger(options.passes) || options.passes < 1 || options.passes > 4))) throw new Error('Passes must be 1 to 4 with the passes mode');
      const begin = await this.request(8, { size: png.length, ...options });
      if (begin.chunk !== 4096 || begin.next !== 0) throw new Error('Invalid display transfer start');
      for (let off = 0; off < png.length; off += 4096) {
        const data = png.subarray(off, off + 4096);
        const r = await this.request(9, { off, data });
        if (r.next !== off + data.length) throw new Error('Invalid display transfer offset; send the picture again');
      }
      return this.finish(await this.request(10, {}));
    });
  }
  private finish(r: CborMap): CborMap {
    if (!['full', 'partial', 'partial_all', 'inverse', 'passes', 'none'].includes(String(r.mode)) ||
        !['asked', 'changed', 'unchanged', 'first', 'refused', 'cut', 'failed', 'memory', 'depth', 'board', 'cleanup', 'large'].includes(String(r.why)) ||
        ![1, 2, 4].includes(Number(r.depth)) || ['ms', 'x', 'y', 'w', 'h'].some((k) => typeof r[k] !== 'number' || !Number.isSafeInteger(r[k]) || Number(r[k]) < 0)) throw new Error('Invalid display answer; check the panel before sending again');
    return r;
  }
  clear(): Promise<CborMap> { return this.exclusive(async () => this.finish(await this.request(11, {}))); }
}
const reasons: Record<string, string> = {
  asked: 'as requested', changed: 'changed pixels', unchanged: 'the picture was already shown', first: 'the first picture',
  refused: 'recovering after a refused transfer', cut: 'recovering after an interrupted transfer', failed: 'recovering after a panel failure',
  memory: 'no memory for the previous frame', depth: 'the bit depth changed', board: 'this panel needs a full refresh', cleanup: 'the periodic full refresh',
  large: 'too much changed for a partial update: refreshed the whole panel',
};
export function displayAnswer(r: CborMap): string {
  const modes: Record<string, string> = { full: 'Full refresh', partial: 'Changed area refreshed', partial_all: 'Whole area refreshed', inverse: 'Inverse then picture refreshed', passes: 'Repeated area refresh', none: 'No refresh needed' };
  return `${modes[String(r.mode)] ?? String(r.mode)}: ${reasons[String(r.why)] ?? String(r.why)} (${r.ms} ms).${r.warn ? ` Check the panel: ${r.warn}.` : ''}${r.ckv && typeof r.ckv === 'object' && 'ok' in r.ckv && r.ckv.ok === false ? ' The panel clock check failed; inspect the panel.' : ''}`;
}
