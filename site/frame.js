// SPDX-License-Identifier: MIT
// The device's wire format, nothing else: CRC-16/CCITT-FALSE, the small CBOR the commands use, and the frames
// (docs/spec/usb-display.md "Frames"). Bytes outside frames are log text and are skipped.

export const MAX_PAYLOAD = 6144;

export function crc16(bytes) {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

const utf8 = new TextEncoder();
const head = (major, n) => {
  if (n < 24) return [(major << 5) | n];
  if (n < 0x100) return [(major << 5) | 24, n];
  if (n < 0x10000) return [(major << 5) | 25, n >> 8, n & 0xff];
  return [(major << 5) | 26, (n >>> 24) & 0xff, (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff];
};

/** One CBOR map with text keys; values: bool, small uint, text, bytes (Uint8Array), a map of these. Anything else is a bug of the caller. */
export function encodeMap(obj) {
  const out = [];
  const keys = Object.keys(obj);
  out.push(...head(5, keys.length));
  for (const k of keys) {
    const kb = utf8.encode(k);
    out.push(...head(3, kb.length), ...kb);
    const v = obj[k];
    if (typeof v === 'boolean') out.push(v ? 0xf5 : 0xf4);
    else if (Number.isInteger(v) && v >= 0 && v <= 0xffffffff) out.push(...head(0, v));
    else if (typeof v === 'string') {
      const vb = utf8.encode(v);
      out.push(...head(3, vb.length), ...vb);
    } else if (v instanceof Uint8Array) {
      out.push(...head(2, v.length));
      for (const x of v) out.push(x);
    } else if (v && typeof v === 'object' && !Array.isArray(v)) out.push(...encodeMap(v)); // a map of the same kinds (set { tuning })
    else throw new Error(`cannot encode ${k}`);
  }
  return Uint8Array.from(out);
}

/** Decode one CBOR item at `pos`; returns [value, next]. Arrays, maps, text, bytes, uints up to 2^53, simple values. */
function item(b, pos) {
  if (pos >= b.length) throw new Error('cbor: short');
  const major = b[pos] >> 5;
  const info = b[pos] & 31;
  let p = pos + 1;
  let n = info;
  if (info >= 24 && info <= 27 && major !== 7) {
    const size = 1 << (info - 24);
    if (p + size > b.length) throw new Error('cbor: short');
    n = 0;
    for (let i = 0; i < size; i++) n = n * 256 + b[p + i];
    p += size;
  } else if (info > 27 && major !== 7) throw new Error('cbor: unsupported');
  switch (major) {
    case 0: return [n, p];
    case 1: return [-1 - n, p];
    case 2:
    case 3: {
      if (p + n > b.length) throw new Error('cbor: short');
      const raw = b.subarray(p, p + n);
      return [major === 3 ? new TextDecoder().decode(raw) : raw.slice(), p + n];
    }
    case 4: {
      const arr = [];
      for (let i = 0; i < n; i++) { let v; [v, p] = item(b, p); arr.push(v); }
      return [arr, p];
    }
    case 5: {
      const map = {};
      for (let i = 0; i < n; i++) {
        let k, v;
        [k, p] = item(b, p);
        [v, p] = item(b, p);
        map[k] = v;
      }
      return [map, p];
    }
    case 7:
      if (info === 20) return [false, p];
      if (info === 21) return [true, p];
      if (info === 22 || info === 23) return [null, p];
      throw new Error('cbor: unsupported');
    default: throw new Error('cbor: unsupported');
  }
}

/** The map a payload holds; an empty payload is `{}`. Throws when it is not one map. */
export function decodeMap(payload) {
  if (!payload.length) return {};
  const [v, end] = item(payload, 0);
  if (!v || typeof v !== 'object' || Array.isArray(v) || end !== payload.length) throw new Error('cbor: not one map');
  return v;
}

/** A whole frame: sync, len, seq, cmd, payload, CRC. */
export function encodeFrame(seq, cmd, payload) {
  const f = new Uint8Array(8 + payload.length);
  f.set([0xa5, 0x5a, payload.length & 0xff, payload.length >> 8, seq, cmd]);
  f.set(payload, 6);
  const crc = crc16(f.subarray(2, 6 + payload.length));
  f[6 + payload.length] = crc & 0xff;
  f[7 + payload.length] = crc >> 8;
  return f;
}

/** Collects the device's bytes, in any piece sizes, and hands out the frames in them. */
export class FrameReader {
  /**
   * `onLine(text, at)`: the device's log lines (`# ...\n`, spec "bytes outside frames are log text"), each whole: a line split
   * over reads is joined, and one between two frames is taken as it comes. `at`: the time (ms) the line was completed. Text
   * never disturbs the frames: it is read only from the bytes before a frame's sync, as before.
   */
  constructor(onLine = null, now = Date.now) {
    this.buf = new Uint8Array(0);
    this.onLine = onLine;
    this.now = now;
    this.line = '';
  }

  /** Bytes outside a frame: log text. Printable ASCII only; the line ends at a newline, or at a frame, or when too long. */
  text(bytes) {
    for (const c of bytes) {
      if (c === 10) this.flush();
      else if (c >= 0x20 && c <= 0x7e) { this.line += String.fromCharCode(c); if (this.line.length >= 400) this.flush(); }
      else if (c >= 0x80) this.line += '?';
    }
  }

  flush() {
    const t = this.line.replace(/^# ?/, '').trimEnd();
    this.line = '';
    if (t && this.onLine) this.onLine(t, this.now());
  }

  push(chunk) {
    const b = new Uint8Array(this.buf.length + chunk.length);
    b.set(this.buf);
    b.set(chunk, this.buf.length);
    this.buf = b;
  }

  /** Forget the bytes waiting; the log lines in them are still passed on. */
  clear() {
    while (this.next());
    this.flush();
    this.buf = new Uint8Array(0);
  }

  /** The next good frame { seq, cmd, payload } or null when more bytes are needed. Log text and bad frames are dropped. */
  next() {
    for (;;) {
      let i = 0;
      while (i + 1 < this.buf.length && !(this.buf[i] === 0xa5 && this.buf[i + 1] === 0x5a)) i++;
      // keep a trailing A5: it may be the start of a sync
      if (i + 1 >= this.buf.length) {
        const keep = this.buf.length && this.buf[this.buf.length - 1] === 0xa5 ? this.buf.length - 1 : this.buf.length;
        this.text(this.buf.subarray(0, keep));
        this.buf = this.buf.subarray(keep);
        return null;
      }
      this.text(this.buf.subarray(0, i));
      if (i > 0) this.flush(); // a frame starts: a line before it is whole
      this.buf = this.buf.subarray(i);
      if (this.buf.length < 6) return null;
      const len = this.buf[2] | (this.buf[3] << 8);
      if (len > MAX_PAYLOAD) { this.buf = this.buf.subarray(2); continue; }
      if (this.buf.length < 8 + len) return null;
      const crc = this.buf[6 + len] | (this.buf[7 + len] << 8);
      if (crc !== crc16(this.buf.subarray(2, 6 + len))) { this.buf = this.buf.subarray(2); continue; }
      const frame = { seq: this.buf[4], cmd: this.buf[5], payload: this.buf.slice(6, 6 + len) };
      this.buf = this.buf.subarray(8 + len);
      return frame;
    }
  }
}
