// Small fake display firmware for the browser guard and the pages' preview. Real protocol behavior is covered by the
// shared vectors.
import { cborEncode, type CborMap } from '../protocol/cbor.ts';
import { FrameParser, decodePayload, encodeFrame } from '../protocol/link-frame.ts';

/** The panels a preview can pretend to be (usb-display.md "Boards"). */
export const MOCK_PANELS = { x: 'TRMNL X', og: 'TRMNL OG', picpak: 'PicPak' } as const;
export type MockPanel = keyof typeof MOCK_PANELS;
export class MockDisplayDevice {
  readonly hello: { proto: number; project: string; fw: string; board: string; w: number; h: number; depths: number[]; colors?: string[];
    partial_depths: number[]; modes: string[]; full_every: number; max_png: number; max_payload: number; chunk: number } = {
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
  /** `x`, `og`, `picpak`: that board's panel as its hello describes it (usb-display.md "Boards"); else a 16 x 8 test panel. */
  constructor(panel = '') {
    if (panel === 'x') Object.assign(this.hello, { board: 'trmnl_x', w: 1872, h: 1404, depths: [1, 2, 4], partial_depths: [1, 2, 4], max_png: 1_319_644,
      modes: ['full', 'partial', 'partial_all', 'inverse', 'passes'] });
    else if (panel === 'og') Object.assign(this.hello, { w: 800, h: 480, max_png: 131_072, modes: ['full', 'partial', 'inverse'] });
    else if (panel === 'picpak') Object.assign(this.hello, { board: 'picpak', w: 400, h: 300, depths: [1], colors: ['black', 'white', 'red', 'yellow'],
      partial_depths: [], modes: ['full'], full_every: 0, max_png: 65_536 });
  }
  onOutput(cb: (b: Uint8Array) => void): void { this.output = cb; }
  write(bytes: Uint8Array): void {
    for (const ev of this.parser.push(bytes)) {
      if (ev.type !== 'frame') continue;
      const { seq, cmd, payload } = ev.frame;
      const req = decodePayload(payload);
      this.calls.push({ cmd, req });
      let res: object;
      if (cmd === 1) res = this.hello;
      else if (this.error) { res = { err: this.error, msg: 'fake refusal', ...(this.detail === undefined ? {} : { detail: this.detail }) }; this.error = 0; }
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
          else if (b.mode === 'partial') res = { mode: 'partial', why: 'changed', depth: show.bits, ms: 0, x: b.x ?? 0, y: b.y ?? 0, w: show.w, h: show.h };
        }
      } else res = { err: 1, msg: 'not a display command' };
      setTimeout(() => this.output(encodeFrame({ seq, cmd: cmd | 128, payload: cborEncode(res) })), 1);
    }
  }
}
