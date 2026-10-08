// Link protocol v1 (link-protocol.md "Frames"): the framing alone, with no command table, so a small client (the
// display's pages) can use it without the app's commands.

import { crc16 } from './bytes.ts';
import { cborDecodeMap, type CborMap } from './cbor.ts';

export const SYNC0 = 0xa5;
export const SYNC1 = 0x5a;
export const MAX_PAYLOAD = 6144;
export const PROTO_VERSION = 1;

export interface Frame {
  seq: number;
  cmd: number;
  payload: Uint8Array;
}

export function encodeFrame(f: Frame): Uint8Array {
  if (f.payload.length > MAX_PAYLOAD) throw new RangeError(`payload ${f.payload.length} > ${MAX_PAYLOAD}`);
  const out = new Uint8Array(8 + f.payload.length);
  out[0] = SYNC0;
  out[1] = SYNC1;
  out[2] = f.payload.length & 0xff;
  out[3] = f.payload.length >> 8;
  out[4] = f.seq & 0xff;
  out[5] = f.cmd & 0xff;
  out.set(f.payload, 6);
  const c = crc16(out.subarray(2, 6 + f.payload.length));
  out[6 + f.payload.length] = c & 0xff;
  out[7 + f.payload.length] = c >> 8;
  return out;
}

export type ParserEvent = { type: 'frame'; frame: Frame } | { type: 'text'; text: string };

/** Streaming parser: feed arbitrary chunks; yields frames and console text (bytes outside frames). */
export class FrameParser {
  private buf = new Uint8Array(0);
  private textDecoder = new TextDecoder('utf-8', { fatal: false });

  push(chunk: Uint8Array): ParserEvent[] {
    const b = new Uint8Array(this.buf.length + chunk.length);
    b.set(this.buf);
    b.set(chunk, this.buf.length);
    const out: ParserEvent[] = [];
    let i = 0;
    let textStart = 0;
    const flushText = (end: number): void => {
      if (end > textStart) out.push({ type: 'text', text: this.textDecoder.decode(b.subarray(textStart, end), { stream: true }) });
    };
    while (i < b.length) {
      if (b[i] !== SYNC0) {
        i++;
        continue;
      }
      if (i + 1 >= b.length) break; // need more to decide
      if (b[i + 1] !== SYNC1) {
        i++;
        continue;
      }
      if (i + 6 > b.length) break;
      const len = b[i + 2]! | (b[i + 3]! << 8);
      if (len > MAX_PAYLOAD) {
        i++; // drop the first sync byte and rescan
        continue;
      }
      if (i + 8 + len > b.length) break;
      const crc = b[i + 6 + len]! | (b[i + 7 + len]! << 8);
      if (crc16(b.subarray(i + 2, i + 6 + len)) !== crc) {
        i++;
        continue;
      }
      flushText(i);
      out.push({ type: 'frame', frame: { seq: b[i + 4]!, cmd: b[i + 5]!, payload: b.slice(i + 6, i + 6 + len) } });
      i += 8 + len;
      textStart = i;
    }
    // Keep a possible frame start; emit text before it.
    let keep = i;
    if (i >= b.length) keep = b.length;
    flushText(Math.min(keep, b.length));
    this.buf = b.slice(keep);
    // bytes kept are re-scanned next time; text before them was flushed
    return out;
  }
}

export function decodePayload(p: Uint8Array): CborMap {
  return p.length === 0 ? {} : cborDecodeMap(p);
}
