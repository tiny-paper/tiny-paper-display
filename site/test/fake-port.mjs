// SPDX-License-Identifier: MIT
// A Web Serial port as the Bluetooth setup uses it, answering like the display firmware from the spec's vectors.
// Not a test file (the runner takes *.test.mjs). Its own CRC, so the tests do not trust the code under test.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const VECTORS = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'docs/spec/vectors/usb-display.json');
export const steps = Object.fromEntries(JSON.parse(readFileSync(VECTORS, 'utf8')).bluetooth.steps.map((s) => [s.name, s]));

export const fromHex = (h) => Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16)));
export const toHex = (b) => [...b].map((x) => x.toString(16).padStart(2, '0')).join('');

export function crc(bytes) {
  let c = 0xffff;
  for (const b of bytes) { c ^= b << 8; for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; }
  return c;
}

/** A frame with this payload (hex), as the device would send it. */
export function frameHex(seq, cmd, payloadHex) {
  const p = fromHex(payloadHex || '');
  const body = Uint8Array.from([p.length & 255, p.length >> 8, seq, cmd, ...p]);
  const c = crc(body);
  return `a55a${toHex(body)}${(c & 255).toString(16).padStart(2, '0')}${(c >> 8).toString(16).padStart(2, '0')}`;
}

/**
 * `answer(txHex)` returns the chunks (hex strings) the device sends back for a request, or nothing for silence.
 * `port.sent`: the requests (hex) in order. `port.closeNow()`: the cable is pulled.
 */
export function fakePort(answer, { openFails = false } = {}) {
  let ctl;
  const port = {
    sent: [],
    opened: 0,
    readable: null,
    writable: null,
    async open() {
      if (openFails) throw new Error('busy');
      port.opened++;
      port.readable = new ReadableStream({ start(c) { ctl = c; }, cancel() { ctl = null; } });
      port.writable = new WritableStream({
        write(chunk) {
          const tx = toHex(chunk);
          port.sent.push(tx);
          for (const c of answer(tx) || []) ctl?.enqueue(fromHex(c));
        },
      });
    },
    async close() { port.readable = null; port.writable = null; },
    push(hex) { ctl?.enqueue(fromHex(hex)); },
    closeNow() { try { ctl?.close(); } catch { /* closed */ } },
  };
  return port;
}

/** A device that answers each vector request with the vector's answer. */
export const vectorDevice = (extra = {}) => {
  const table = { ...Object.fromEntries(Object.values(steps).filter((s) => s.link === 'usb').map((s) => [s.tx, [s.rx]])), ...extra };
  return (tx) => table[tx];
};
