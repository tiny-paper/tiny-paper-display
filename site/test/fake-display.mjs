// SPDX-License-Identifier: MIT
// A display firmware behind the fake serial port, for the Sketch page: hello, show (begin, chunks, end) and what it was
// asked. Its own CBOR writer and the fake port's own CRC, so the tests do not trust the bundle under test; requests
// are read with the site's decoder (frame.js, checked against the spec's vectors in bt-setup.test.mjs).
// Not a test file (the runner takes *.test.mjs).
import { decodeMap } from '../frame.js';
import { fakePort, frameHex, fromHex, toHex } from './fake-port.mjs';

const head = (major, n) => (n < 24 ? [(major << 5) | n] : n < 256 ? [(major << 5) | 24, n] : n < 65536 ? [(major << 5) | 25, n >> 8, n & 255]
  : [(major << 5) | 26, (n >>> 24) & 255, (n >> 16) & 255, (n >> 8) & 255, n & 255]);
function cbor(v) {
  if (typeof v === 'boolean') return [v ? 0xf5 : 0xf4];
  if (Number.isInteger(v) && v >= 0) return head(0, v);
  if (typeof v === 'string') { const b = [...new TextEncoder().encode(v)]; return [...head(3, b.length), ...b]; }
  if (Array.isArray(v)) return [...head(4, v.length), ...v.flatMap(cbor)];
  const keys = Object.keys(v);
  return [...head(5, keys.length), ...keys.flatMap((k) => [...cbor(k), ...cbor(v[k])])];
}

export const HELLO = {
  proto: 1, project: 'display', fw: '0.0.0+test', board: 'trmnl_og', w: 800, h: 480, depths: [1, 2], partial_depths: [1],
  modes: ['full', 'partial', 'inverse'], full_every: 20, max_png: 131072, max_payload: 6144, chunk: 4096,
};

// The TRMNL X's knobs as docs/spec/usb-display.md "Tuning" lists them: [name, min, max, default, words?]
const KNOBS = [
  ['gray_table', 0, 1, 1, ['fastepd', 'stock']], ['gray_partial', 0, 1, 1, ['off', 'on']], ['gray_dark', 0, 14, 0], ['gray_light', 0, 14, 0],
  ['passes_partial', 1, 14, 4], ['passes_full', 1, 14, 3], ['wave_kick', 0, 14, 2], ['wave_extra', 0, 14, 0], ['wave_redrive', 0, 3, 2],
  ['row_settle', 0, 20, 2], ['row_hold', 0, 20, 2], ['row_drive', 0, 200, 32], ['clear', 0, 3, 0, ['auto', 'none', 'fast', 'slow']],
  ['gray_row', 0, 0, 0, null, true], ['step_neutral', 0, 1, 0, ['each', 'end']], ['row_skip', 0, 1, 0, ['off', 'on']], ['row_skip_us', 2, 50, 10], ['scan_min_ms', 0, 150, 0],
  ['frame_work', 0, 1, 0, ['whole', 'rows']],
];
const tuningOf = (vals) => Object.fromEntries(KNOBS.map(([n, , , , w], i) => [n, w ? w[vals[i]] : vals[i]]));
const start = (k) => (k[5] ? '' : k[3]);

/**
 * { port, shows, hello, refuse, sets, knobs }: `shows` holds every finished show ({ mode, x, y, w, h, bits } with w, h and bits read
 * from the PNG it received). `refuse = { err, msg }`: the next show ends with that refusal. `hello: null`: silence.
 */
export function fakeDisplay(over = {}) {
  const knobs = KNOBS.map(start);
  const dev = { hello: over === null ? null : { ...HELLO, ...over }, shows: [], pngs: [], refusals: [], refuse: null, port: null, sets: [], knobs, log: {} };
  if (dev.hello && dev.hello.tuning === true) dev.hello.tuning = tuningOf(knobs); // `tuning: true`: a board with the knobs
  let begin = {};
  let png = new Uint8Array();
  dev.port = fakePort((tx) => {
    const b = fromHex(tx);
    const seq = b[4];
    const cmd = b[5];
    const req = decodeMap(b.subarray(6, b.length - 2));
    let res;
    if (cmd === 1) res = dev.hello && dev.hello.tuning ? { ...dev.hello, tuning: tuningOf(knobs) } : dev.hello;
    else if (cmd === 12) {
      dev.sets.push(req);
      const refused = {};
      if (!dev.hello.tuning) { if (req.tuning !== undefined) refused.tuning = 'this board has no panel tuning'; }
      else if (req.tuning === 'defaults') KNOBS.forEach((k, i) => { knobs[i] = start(k); });
      else if (req.tuning !== undefined) {
        for (const [key, v] of Object.entries(req.tuning)) {
          const i = KNOBS.findIndex((k) => k[0] === key);
          if (i < 0) { refused[key] = `no such key; known: ${KNOBS.map((k) => k[0]).join(' ')}`; continue; }
          if (KNOBS[i][5]) { knobs[i] = typeof v === 'string' ? v : knobs[i]; continue; } // gray_row: the real rules are the mock's, not this one's
          const [, min, max, , words] = KNOBS[i];
          const range = words ? `allowed ${words.join(', ')}` : `allowed ${min} to ${max}`;
          const n = typeof v === 'number' ? v : typeof v === 'boolean' && words ? Number(v) : words && words.includes(v) ? words.indexOf(v) + min : undefined;
          if (n === undefined) refused[key] = `${words ? 'not one of the words' : 'not a whole number'}; ${range}`;
          else if (n < min || n > max) refused[key] = `out of range; ${range}`;
          else knobs[i] = n;
        }
      }
      res = { full_every: dev.hello.full_every, ...(dev.hello.tuning ? { tuning: tuningOf(knobs) } : {}), ...(Object.keys(refused).length ? { refused } : {}) };
    }
    else if (cmd === 8) { begin = req; png = new Uint8Array(req.size); res = { next: 0, chunk: 4096 }; }
    else if (cmd === 9) { png.set(req.data, req.off); res = { next: req.off + req.data.length }; }
    else if (cmd === 10 && dev.refuse) { res = dev.refuse; dev.refuse = null; }
    else if (cmd === 10) {
      const v = new DataView(png.buffer);
      const show = { mode: begin.mode, ...(begin.passes === undefined ? {} : { passes: begin.passes }), x: begin.x, y: begin.y, w: v.getUint32(16), h: v.getUint32(20), bits: png[24], signature: toHex(png.subarray(0, 8)) };
      // `dev.keeps` (a depth, or null for none) models the frame the firmware keeps (usbdisp_area.c): a rectangle at another depth
      // is refused DEPTH, and the frame is forgotten; a rectangle without a frame is a full refresh; a whole picture is kept
      const area = begin.x !== undefined;
      let forced = null;
      if (dev.keeps !== undefined && area) {
        if (dev.keeps !== null && dev.keeps !== show.bits) {
          res = { err: 34, msg: `the picture shown has ${dev.keeps} bit and the rectangle ${show.bits} bit: an area must have the same depth` };
          dev.keeps = null; dev.refusals.push(res.msg);
          return [frameHex(seq, cmd | 0x80, toHex(Uint8Array.from(cbor(res))))];
        }
        if (dev.keeps === null) forced = 'first';
      }
      if (dev.keeps !== undefined && (!area || forced)) dev.keeps = show.bits;
      dev.shows.push(show);
      dev.pngs.push(Uint8Array.from(png));
      res = forced ? { mode: 'full', why: forced, depth: show.bits, ms: 9, x: 0, y: 0, w: dev.hello.w, h: dev.hello.h } : begin.mode && begin.mode !== 'full'
        ? { mode: begin.mode, why: begin.mode === 'partial' ? 'changed' : 'asked', depth: show.bits, ms: 7, x: begin.x ?? 0, y: begin.y ?? 0, w: show.w, h: show.h }
        : { mode: 'full', why: 'asked', depth: show.bits, ms: 9, x: 0, y: 0, w: dev.hello.w, h: dev.hello.h };
    } else res = { err: 1, msg: 'not a display command' };
    if (!res) return [];
    // the device's log text: `dev.log[cmd] = { before: [...], after: [...], split: true }`, lines without the "# " and newline
    const text = (lines) => lines.map((l) => toHex(new TextEncoder().encode(`# ${l}\n`)));
    const log = dev.log[cmd] ?? {};
    let before = text(log.before ?? []);
    if (log.split && before.length) { const h = before[0]; before = [h.slice(0, 14), h.slice(14), ...before.slice(1)]; } // cut a line in two reads
    return [...before, frameHex(seq, cmd | 0x80, toHex(Uint8Array.from(cbor(res)))), ...text(log.after ?? [])];
  });
  return dev;
}
