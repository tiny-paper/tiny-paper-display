// CBOR per docs/spec/README.md "CBOR profile": definite lengths, shortest-form heads,
// text map keys, no tags, no duplicate keys, nesting <= 8.
//
// Own codec instead of cbor-x: cbor-x encodes integers >= 2^32 as float64 (wrong for
// `unix_ms`) and its decoder accepts tags, indefinite lengths and duplicate keys, which
// the profile refuses. Tests cross-check against cbor-x and the Python generator.

export type CborValue =
  | null
  | boolean
  | number
  | bigint
  | string
  | Uint8Array
  | CborValue[]
  | { [key: string]: CborValue };

export type CborMap = { [key: string]: CborValue };

export class CborError extends Error {
  override name = 'CborError';
}

const MAX_DEPTH = 8;
/** Key work of one item (docs/spec/README.md "CBOR profile", C `CBOR_KEY_WORK_MAX`): for every map key the items
 * before it in its map (earlier keys, their values and all inside those), summed over all maps. More is invalid: the
 * device's duplicate-key check costs this many reads. */
export const CBOR_KEY_WORK_MAX = 4194304;
const te = new TextEncoder();
const td = new TextDecoder('utf-8', { fatal: true });

class Writer {
  buf = new Uint8Array(256);
  len = 0;
  /** Items written so far, and the key work (CBOR_KEY_WORK_MAX). */
  items = 0;
  work = 0;
  /** Before a key of the map whose pairs start after item `start`. */
  key(start: number): void {
    this.work += this.items - start;
    if (this.work > CBOR_KEY_WORK_MAX) throw new CborError('map key work over the limit');
  }
  ensure(n: number): void {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const b = new Uint8Array(cap);
    b.set(this.buf.subarray(0, this.len));
    this.buf = b;
  }
  byte(b: number): void {
    this.ensure(1);
    this.buf[this.len++] = b;
  }
  bytes(b: Uint8Array): void {
    this.ensure(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
  }
  head(major: number, n: number | bigint): void {
    const m = major << 5;
    if (typeof n === 'bigint') {
      if (n < 0n || n > 0xffffffffffffffffn) throw new CborError('integer out of range');
      if (n <= 0xffffffffn) return this.head(major, Number(n));
      this.byte(m | 27);
      const v = new DataView(new ArrayBuffer(8));
      v.setBigUint64(0, n);
      this.bytes(new Uint8Array(v.buffer));
      return;
    }
    if (n < 24) this.byte(m | n);
    else if (n < 0x100) {
      this.byte(m | 24);
      this.byte(n);
    } else if (n < 0x10000) {
      this.byte(m | 25);
      this.byte(n >> 8);
      this.byte(n & 0xff);
    } else if (n < 0x100000000) {
      this.byte(m | 26);
      this.byte((n >>> 24) & 0xff);
      this.byte((n >>> 16) & 0xff);
      this.byte((n >>> 8) & 0xff);
      this.byte(n & 0xff);
    } else {
      this.head(major, BigInt(n));
    }
  }
}

function encodeInto(w: Writer, v: unknown, depth: number, short = false): void {
  if (depth > MAX_DEPTH) throw new CborError('nesting deeper than 8');
  w.items++;
  if (v === null) return w.byte(0xf6);
  if (v === true) return w.byte(0xf5);
  if (v === false) return w.byte(0xf4);
  if (typeof v === 'number') {
    if (Number.isInteger(v) && !Object.is(v, -0)) {
      if (!Number.isSafeInteger(v)) {
        w.items--;
        return encodeInto(w, BigInt(v), depth);
      }
      return v >= 0 ? w.head(0, v) : w.head(1, -1 - v);
    }
    if (short) {
      // The shortest float that holds the value exactly (README: decoders accept 16, 32 and 64 bit)
      const h = floatToHalf(v);
      if (h >= 0) return w.bytes(Uint8Array.of(0xf9, h >> 8, h & 0xff));
      if (Math.fround(v) === v || Number.isNaN(v)) {
        const d4 = new DataView(new ArrayBuffer(4));
        d4.setFloat32(0, v);
        w.byte(0xfa);
        return w.bytes(new Uint8Array(d4.buffer));
      }
    }
    w.byte(0xfb);
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, v);
    return w.bytes(new Uint8Array(dv.buffer));
  }
  if (typeof v === 'bigint') return v >= 0n ? w.head(0, v) : w.head(1, -1n - v);
  if (typeof v === 'string') {
    const b = te.encode(v);
    w.head(3, b.length);
    return w.bytes(b);
  }
  if (v instanceof Uint8Array) {
    w.head(2, v.length);
    return w.bytes(v);
  }
  if (Array.isArray(v)) {
    w.head(4, v.length);
    for (const x of v) encodeInto(w, x, depth + 1, short);
    return;
  }
  if (v instanceof Map) {
    w.head(5, v.size);
    const start = w.items;
    for (const [k, x] of v) {
      if (typeof k !== 'string') throw new CborError('map keys must be strings');
      w.key(start);
      encodeInto(w, k, depth + 1, short);
      encodeInto(w, x, depth + 1, short);
    }
    return;
  }
  if (typeof v === 'object') {
    const entries = Object.entries(v as object).filter(([, x]) => x !== undefined);
    w.head(5, entries.length);
    const start = w.items;
    for (const [k, x] of entries) {
      w.key(start);
      encodeInto(w, k, depth + 1, short);
      encodeInto(w, x, depth + 1, short);
    }
    return;
  }
  throw new CborError(`cannot encode ${typeof v}`);
}

/** Encode (map keys keep insertion order; `undefined` members are skipped). */
export function cborEncode(v: unknown): Uint8Array {
  const w = new Writer();
  encodeInto(w, v, 1);
  return w.buf.slice(0, w.len);
}

/** As cborEncode, each float in the shortest of float16, float32 and float64 that holds it exactly (smaller consts). */
export function cborEncodeShort(v: unknown): Uint8Array {
  const w = new Writer();
  encodeInto(w, v, 1, true);
  return w.buf.slice(0, w.len);
}

/** The float16 bits of v when float16 holds it exactly (finite, normal or zero), else -1. */
function floatToHalf(v: number): number {
  if (!Number.isFinite(v)) return -1;
  if (v === 0) return Object.is(v, -0) ? 0x8000 : 0;
  const a = Math.abs(v);
  const e = Math.floor(Math.log2(a));
  if (e < -14 || e > 15) return -1;
  const m = a / 2 ** e - 1;
  const f = m * 1024;
  if (!Number.isInteger(f) || f < 0 || f > 1023 || (1 + f / 1024) * 2 ** e !== a) return -1;
  return (v < 0 ? 0x8000 : 0) | ((e + 15) << 10) | f;
}

function halfToFloat(h: number): number {
  const s = h & 0x8000 ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return s * f * 2 ** -24;
  if (e === 31) return f ? NaN : s * Infinity;
  return s * (1 + f / 1024) * 2 ** (e - 15);
}

class Reader {
  pos = 0;
  readonly b: Uint8Array;
  constructor(b: Uint8Array) {
    this.b = b;
  }
  need(n: number): void {
    if (this.pos + n > this.b.length) throw new CborError('truncated');
  }
  u8(): number {
    this.need(1);
    return this.b[this.pos++]!;
  }
  arg(ai: number): number | bigint {
    const dv = new DataView(this.b.buffer, this.b.byteOffset, this.b.byteLength);
    let v: number | bigint;
    if (ai < 24) return ai;
    if (ai === 24) {
      v = this.u8();
      if (v < 24) throw new CborError('non-shortest integer');
    } else if (ai === 25) {
      this.need(2);
      v = dv.getUint16(this.pos);
      this.pos += 2;
      if (v < 0x100) throw new CborError('non-shortest integer');
    } else if (ai === 26) {
      this.need(4);
      v = dv.getUint32(this.pos);
      this.pos += 4;
      if (v < 0x10000) throw new CborError('non-shortest integer');
    } else if (ai === 27) {
      this.need(8);
      const big = dv.getBigUint64(this.pos);
      this.pos += 8;
      if (big < 0x100000000n) throw new CborError('non-shortest integer');
      v = big <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(big) : big;
    } else if (ai === 31) throw new CborError('indefinite length');
    else throw new CborError('reserved additional info');
    return v;
  }
  len(ai: number): number {
    const n = this.arg(ai);
    if (typeof n === 'bigint' || n > this.b.length) throw new CborError('length too large');
    return n;
  }
  /** Items read so far, and the key work (CBOR_KEY_WORK_MAX). */
  items = 0;
  work = 0;
  item(depth: number): CborValue {
    if (depth > MAX_DEPTH) throw new CborError('nesting deeper than 8');
    this.items++;
    const ib = this.u8();
    const major = ib >> 5;
    const ai = ib & 0x1f;
    switch (major) {
      case 0:
        return this.arg(ai);
      case 1: {
        const n = this.arg(ai);
        if (typeof n === 'bigint') return -1n - n;
        const r = -1 - n;
        return Number.isSafeInteger(r) ? r : -1n - BigInt(n);
      }
      case 2: {
        const n = this.len(ai);
        this.need(n);
        const out = this.b.slice(this.pos, this.pos + n);
        this.pos += n;
        return out;
      }
      case 3: {
        const n = this.len(ai);
        this.need(n);
        let s: string;
        try {
          s = td.decode(this.b.subarray(this.pos, this.pos + n));
        } catch {
          throw new CborError('invalid UTF-8');
        }
        this.pos += n;
        return s;
      }
      case 4: {
        const n = this.len(ai);
        const out: CborValue[] = [];
        for (let i = 0; i < n; i++) out.push(this.item(depth + 1));
        return out;
      }
      case 5: {
        const n = this.len(ai);
        const out: CborMap = {};
        const seen = new Set<string>();
        const start = this.items;
        for (let i = 0; i < n; i++) {
          this.work += this.items - start;
          if (this.work > CBOR_KEY_WORK_MAX) throw new CborError('map key work over the limit');
          const k = this.item(depth + 1);
          if (typeof k !== 'string') throw new CborError('map key is not a text string');
          if (seen.has(k)) throw new CborError(`duplicate map key "${k}"`);
          seen.add(k);
          const v = this.item(depth + 1);
          Object.defineProperty(out, k, { value: v, enumerable: true, writable: true, configurable: true });
        }
        return out;
      }
      case 6:
        throw new CborError('tags are not allowed');
      default: {
        if (ai === 20) return false;
        if (ai === 21) return true;
        if (ai === 22) return null;
        const dv = new DataView(this.b.buffer, this.b.byteOffset, this.b.byteLength);
        if (ai === 25) {
          this.need(2);
          const v = halfToFloat(dv.getUint16(this.pos));
          this.pos += 2;
          return v;
        }
        if (ai === 26) {
          this.need(4);
          const v = dv.getFloat32(this.pos);
          this.pos += 4;
          return v;
        }
        if (ai === 27) {
          this.need(8);
          const v = dv.getFloat64(this.pos);
          this.pos += 8;
          return v;
        }
        if (ai === 31) throw new CborError('indefinite length');
        throw new CborError(`unsupported simple value ${ai}`);
      }
    }
  }
}

/** Strict decode of exactly one item. Maps become plain objects, bstr Uint8Array,
 * integers beyond 2^53 bigint. Throws CborError. */
export function cborDecode(b: Uint8Array): CborValue {
  const r = new Reader(b);
  const v = r.item(1);
  if (r.pos !== b.length) throw new CborError('trailing bytes');
  return v;
}

/** Key work of the item `b` (CBOR_KEY_WORK_MAX; throws as cborDecode). */
export function cborKeyWork(b: Uint8Array): number {
  const r = new Reader(b);
  r.item(1);
  if (r.pos !== b.length) throw new CborError('trailing bytes');
  return r.work;
}

/** Decode a single item from the start of `b`; returns the value and its length. */
export function cborDecodePrefix(b: Uint8Array): { value: CborValue; length: number } {
  const r = new Reader(b);
  const value = r.item(1);
  return { value, length: r.pos };
}

export function isCborMap(v: unknown): v is CborMap {
  return typeof v === 'object' && v !== null && !Array.isArray(v) && !(v instanceof Uint8Array);
}

/** Decode and require a map (link payloads, MANI, SETS). */
export function cborDecodeMap(b: Uint8Array): CborMap {
  const v = cborDecode(b);
  if (!isCborMap(v)) throw new CborError('expected a map');
  return v;
}
