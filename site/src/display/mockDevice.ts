// Small fake display firmware for the browser guard and the pages' preview. Real protocol behavior is covered by the
// shared vectors.
import { cborEncode, type CborMap } from '../protocol/cbor.ts';
import { FrameParser, decodePayload, encodeFrame } from '../protocol/link-frame.ts';

/** The TRMNL X's tuning knobs as the firmware lists them (usb-display.md "Tuning"): range, default, words. */
export const X_KNOBS: readonly { name: string; min: number; max: number; def: number; words?: readonly string[]; text?: true }[] = [
  { name: 'gray_table', min: 0, max: 1, def: 1, words: ['fastepd', 'stock'] },
  { name: 'gray_partial', min: 0, max: 1, def: 1, words: ['off', 'on'] },
  { name: 'gray_dark', min: 0, max: 14, def: 0 }, { name: 'gray_light', min: 0, max: 14, def: 0 },
  { name: 'passes_partial', min: 1, max: 14, def: 4 }, { name: 'passes_full', min: 1, max: 14, def: 3 },
  { name: 'wave_kick', min: 0, max: 14, def: 2 }, { name: 'wave_extra', min: 0, max: 14, def: 0 }, { name: 'wave_redrive', min: 0, max: 3, def: 2 },
  { name: 'row_settle', min: 0, max: 20, def: 2 }, { name: 'row_hold', min: 0, max: 20, def: 2 }, { name: 'row_drive', min: 0, max: 200, def: 32 },
  { name: 'clear', min: 0, max: 3, def: 0, words: ['auto', 'none', 'fast', 'slow'] },
  { name: 'gray_row', min: 0, max: 0, def: 0, text: true }, // `level:passes` entries; the value is the rows that differ, as text
  { name: 'step_neutral', min: 0, max: 1, def: 0, words: ['each', 'end'] }, { name: 'row_skip', min: 0, max: 1, def: 0, words: ['off', 'on'] },
  { name: 'row_skip_us', min: 2, max: 50, def: 10 }, { name: 'scan_min_ms', min: 0, max: 150, def: 0 },
];
/** A knob's value at start: its default, or the empty text. */
export const knobStart = (k: typeof X_KNOBS[number]): number | string => (k.text ? '' : k.def);
/** The knobs as hello and every `set` answer report them: a word where the knob has words, else the number. */
export function tuningMap(values: readonly (number | string)[], knobs = X_KNOBS): Record<string, number | string> {
  return Object.fromEntries(knobs.map((k, i) => [k.name, k.words ? k.words[(values[i] as number) - k.min]! : values[i]!]));
}
/**
 * What the firmware does with `set { tuning }` (usbdisp.c set_tuning): each key on its own, the good ones applied, the others
 * answered under `refused` with why and the allowed range. `values` is changed in place.
 */
export function applyTuning(values: (number | string)[], request: unknown, knobs = X_KNOBS): Record<string, string> {
  const refused: Record<string, string> = {};
  const range = (k: typeof knobs[number]): string => (k.words ? `allowed ${k.words.join(', ')}` : `allowed ${k.min} to ${k.max}`);
  if (request === 'defaults') { knobs.forEach((k, i) => { values[i] = knobStart(k); }); return refused; }
  if (typeof request !== 'object' || request === null || Array.isArray(request)) return { tuning: 'tuning is a map of keys, or "defaults"' };
  const table = knobs.findIndex((k) => k.name === 'gray_table');
  // gray_table first: it is applied before the rows, and a change of it puts every row back
  const entries = Object.entries(request as Record<string, unknown>).sort(([a], [b]) => Number(b === 'gray_table') - Number(a === 'gray_table'));
  for (const [key, v] of entries) {
    if (Object.keys(refused).length >= 8) break;
    const i = knobs.findIndex((k) => k.name === key);
    if (i < 0) { refused[key] = `no such key; known: ${knobs.map((k) => k.name).join(' ')}`.slice(0, 120); continue; }
    const k = knobs[i]!;
    if (k.text) { const why = grayRows(values, i, table, v); if (why) refused[key] = why; continue; }
    let n: number | undefined;
    if (typeof v === 'number' && Number.isInteger(v)) n = v;
    else if (typeof v === 'boolean' && k.words && k.max === 1) n = v ? 1 : 0;
    else if (typeof v === 'string' && k.words) { const w = k.words.indexOf(v); n = w < 0 ? undefined : w + k.min; }
    else if (typeof v === 'object') { refused[key] = 'the value is a number or a word'; continue; }
    if (n === undefined) refused[key] = `${k.words ? 'not one of the words' : 'not a whole number'}; ${range(k)}`;
    else if (n < k.min || n > k.max) refused[key] = `out of range; ${range(k)}`;
    else {
      if (i === table && values[i] !== n) knobs.forEach((x, j) => { if (x.text) values[j] = ''; });
      values[i] = n;
    }
  }
  return refused;
}
/** gray_row: `level:passes` entries (spaces or commas) replace single rows; any bad entry refuses the whole text, in words. */
function grayRows(values: (number | string)[], at: number, table: number, v: unknown): string {
  if (typeof v !== 'string') return 'the value is text: level:passes entries';
  if (v === '' || v === 'defaults') { values[at] = ''; return ''; }
  const stock = values[table] === 1, n = stock ? 9 : 8, name = stock ? 'stock' : 'fastepd';
  const rows = new Map<number, string>();
  for (const e of String(values[at]).split(' ').filter(Boolean)) rows.set(Number(e.split(':')[0]), e.split(':')[1]!);
  for (const e of v.split(/[\s,]+/).filter(Boolean)) {
    const m = /^(\d+):(.*)$/.exec(e);
    if (!m || Number(m[1]) > 15) return `${e}: not level:passes with a level 0 to 15`;
    if (!/^[.DL]*$/.test(m[2]!)) return `level ${m[1]}: a pass is . D or L`;
    if (m[2]!.length !== n) return `level ${m[1]}: ${m[2]!.length} passes given, the ${name} table has ${n}`;
    rows.set(Number(m[1]), m[2]!);
  }
  values[at] = [...rows].sort(([a], [b]) => a - b).map(([l, p]) => `${l}:${p}`).join(' ');
  return '';
}

/** The panels a preview can pretend to be (usb-display.md "Boards"). */
export const MOCK_PANELS = { x: 'TRMNL X', og: 'TRMNL OG', picpak: 'PicPak' } as const;
export type MockPanel = keyof typeof MOCK_PANELS;
export class MockDisplayDevice {
  readonly hello: { proto: number; project: string; fw: string; board: string; w: number; h: number; depths: number[]; colors?: string[];
    partial_depths: number[]; modes: string[]; full_every: number; max_png: number; max_payload: number; chunk: number;
    tuning?: Record<string, number | string> } = {
      proto: 1, project: 'display', fw: 'test', board: 'trmnl_og', w: 16, h: 8,
      depths: [1, 2], partial_depths: [1], modes: ['full', 'partial', 'inverse', 'passes'],
      full_every: 20, max_png: 8192, max_payload: 6144, chunk: 4096 };
  /** Every finished show: what was asked and the PNG's header. */
  readonly shows: { mode?: string; x?: number; y?: number; w: number; h: number; bits: number; png: Uint8Array }[] = [];
  /** One answer as after a lost frame: a full refresh nobody asked for. */
  lost = false;
  detail: unknown = undefined;
  private begin: CborMap = {};
  readonly calls: { cmd: number; req: CborMap }[] = [];
  readonly shown: number[][] = [];
  error = 0;
  private parser = new FrameParser();
  private output: (b: Uint8Array) => void = () => {};
  private png = new Uint8Array();
  private next = 0;
  /** The TRMNL X's knobs, now (`x` panel only). */
  readonly knobs: (number | string)[] = X_KNOBS.map(knobStart);
  /** `x`, `og`, `picpak`: that board's panel as its hello describes it (usb-display.md "Boards"); else a 16 x 8 test panel. */
  constructor(panel = '') {
    if (panel === 'x') Object.assign(this.hello, { board: 'trmnl_x', w: 1872, h: 1404, depths: [1, 2, 4], partial_depths: [1, 2, 4], max_png: 1_319_644,
      modes: ['full', 'partial', 'partial_all', 'inverse', 'passes'], tuning: tuningMap(this.knobs) });
    else if (panel === 'og') Object.assign(this.hello, { w: 800, h: 480, max_png: 131_072, modes: ['full', 'partial', 'inverse'] });
    else if (panel === 'picpak') Object.assign(this.hello, { board: 'picpak', w: 400, h: 300, depths: [1], colors: ['black', 'white', 'red', 'yellow'],
      partial_depths: [], modes: ['full'], full_every: 0, max_png: 65_536 });
  }
  onOutput(cb: (b: Uint8Array) => void): void { this.output = cb; }
  /** The log lines the X writes after a refresh (panel_par.c, formats copied; the numbers are made up): the page's Device log shows what they look like. */
  private logFor(r: CborMap): string[] {
    const y0 = Number(r.y), y1 = y0 + Number(r.h) - 1;
    if (r.mode === 'full') return [`panel full depth=${r.depth} ms=2591 clear=fast gray=stock rowmax=61@12 rc=0`, 'panel rows drive=0us high=29/37/55 n=3 waited=0 absorbed=0 over=0 ckv=8424/6',
      'panel time total=2612 begin=9 write=11 up=0 prep=3 masks=0 lib=61 scans=2520 down=4 after=0 save=0 log=4 other=0 calls=1'];
    if (Number(r.depth) > 1) return [`panel gpart depth=${r.depth} rows=${y0}..${y1} ms=2567 dark=${this.knobs[2]} light=${this.knobs[3]} gray=${this.knobs[0] ? 'stock' : 'fastepd'} steps=4 frames=9 chg=2400 drv=2400 rowmax=88@${y0} rc=0`, 'panel rows drive=32us high=30/37/52 n=281 waited=0 absorbed=0 over=0 ckv=12636/9',
      'panel time total=2590 begin=9 write=2 up=42 prep=1 masks=96 lib=148 scans=2270 down=4 after=6 save=0 log=12 other=0 calls=4'];
    return [`panel partial rows=${y0}..${y1} ms=1465 passes=4 kick=2/90ms main=4/180ms extra=0/0ms redrive=0@0..0 rowdly=2,2 rowmax=61@${y0} fresh=0/0ms since_full=1 rc=0`, 'panel rows drive=32us high=30/37/52 n=281 waited=0 absorbed=0 over=0 ckv=11236/8',
      'panel time total=1490 begin=9 write=1 up=42 prep=58 masks=74 lib=52 scans=1196 down=4 after=31 save=0 log=23 other=0 calls=2'];
  }
  write(bytes: Uint8Array): void {
    for (const ev of this.parser.push(bytes)) {
      if (ev.type !== 'frame') continue;
      const { seq, cmd, payload } = ev.frame;
      const req = decodePayload(payload);
      this.calls.push({ cmd, req });
      let res: object;
      if (cmd === 1) res = this.hello.tuning ? { ...this.hello, tuning: tuningMap(this.knobs) } : this.hello;
      else if (cmd === 12 && !this.error) {
        const refused = req.tuning === undefined ? {} : this.hello.tuning ? applyTuning(this.knobs, req.tuning) : { tuning: 'this board has no panel tuning' };
        res = { full_every: this.hello.full_every, ...(this.hello.tuning ? { tuning: tuningMap(this.knobs) } : {}), ...(Object.keys(refused).length ? { refused } : {}) };
      } else if (this.error) { res = { err: this.error, msg: 'fake refusal', ...(this.detail === undefined ? {} : { detail: this.detail }) }; this.error = 0; }
      else if (cmd === 8) { this.png = new Uint8Array(Number(req.size)); this.next = 0; this.begin = req; res = { next: 0, chunk: 4096 }; }
      else if (cmd === 9) {
        const data = req.data as Uint8Array;
        this.png.set(data, Number(req.off)); this.next += data.length; res = { next: this.next };
      } else if (cmd === 10 || cmd === 11) {
        res = { mode: 'full', why: 'asked', depth: 1, ms: 0, x: 0, y: 0, w: this.hello.w, h: this.hello.h };
        if (cmd === 10) {
          this.shown.push(Array.from(this.png));
          const v = new DataView(this.png.buffer), b = this.begin as { mode?: string; x?: number; y?: number };
          const show = { ...(b.mode === undefined ? {} : { mode: b.mode }), ...(b.x === undefined ? {} : { x: b.x, y: b.y }), w: v.getUint32(16), h: v.getUint32(20), bits: this.png[24]!, png: this.png };
          this.shows.push(show);
          if (this.lost) { this.lost = false; res = { ...res, why: 'cut' }; }
          else if (b.mode && b.mode !== 'full') res = { mode: b.mode, why: b.mode === 'partial' ? 'changed' : 'asked', depth: show.bits, ms: 0, x: b.x ?? 0, y: b.y ?? 0, w: show.w, h: show.h };
        }
      } else res = { err: 1, msg: 'not a display command' };
      const log = cmd === 10 && this.hello.tuning ? this.logFor(res as CborMap) : [];
      setTimeout(() => {
        const text = new TextEncoder().encode(log.map((l) => `# ${l}\n`).join(''));
        if (text.length > 20) { this.output(text.subarray(0, 20)); this.output(text.subarray(20)); } else if (text.length) this.output(text); // a line split over two reads
        this.output(encodeFrame({ seq, cmd: cmd | 128, payload: cborEncode(res) }));
      }, 1);
    }
  }
}
