// Checksums and byte encodings shared by every format.

const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** CRC-32/ISO-HDLC (zlib). Streaming: crc32(b, crc32(a)). */
export function crc32(data: Uint8Array, crc = 0): number {
  let c = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) c = CRC32_TABLE[(c ^ data[i]!) & 0xff]! ^ (c >>> 8);
  return ~c >>> 0;
}

/** CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Streaming: pass the previous value. */
export function crc16(data: Uint8Array, crc = 0xffff): number {
  let c = crc;
  for (let i = 0; i < data.length; i++) {
    c ^= data[i]! << 8;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
  }
  return c;
}

export function toHex(b: Uint8Array): string {
  let s = '';
  for (const x of b) s += x.toString(16).padStart(2, '0');
  return s;
}

export function fromHex(h: string): Uint8Array {
  const s = h.replace(/\s+/g, '');
  if (s.length % 2 || /[^0-9a-fA-F]/.test(s)) throw new Error('invalid hex');
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(s.slice(i * 2, i * 2 + 2), 16);
  return out;
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

/** base64url without padding. */
export function toB64u(b: Uint8Array): string {
  let s = '';
  for (let i = 0; i < b.length; i += 3) {
    const n = (b[i]! << 16) | ((b[i + 1] ?? 0) << 8) | (b[i + 2] ?? 0);
    s += B64[(n >> 18) & 63]! + B64[(n >> 12) & 63]!;
    if (i + 1 < b.length) s += B64[(n >> 6) & 63]!;
    if (i + 2 < b.length) s += B64[n & 63]!;
  }
  return s;
}

export function fromB64u(s: string): Uint8Array {
  const clean = s.replace(/=+$/, '').replace(/\+/g, '-').replace(/\//g, '_');
  if (/[^A-Za-z0-9_-]/.test(clean) || clean.length % 4 === 1) throw new Error('invalid base64url');
  const out = new Uint8Array(Math.floor((clean.length * 3) / 4));
  let o = 0;
  for (let i = 0; i < clean.length; i += 4) {
    const c = [0, 1, 2, 3].map((k) => (i + k < clean.length ? B64.indexOf(clean[i + k]!) : 0));
    const n = (c[0]! << 18) | (c[1]! << 12) | (c[2]! << 6) | c[3]!;
    out[o++] = (n >> 16) & 0xff;
    if (i + 2 < clean.length) out[o++] = (n >> 8) & 0xff;
    if (i + 3 < clean.length) out[o++] = n & 0xff;
  }
  return out;
}

const B32 = 'abcdefghijklmnopqrstuvwxyz234567';

/** RFC 4648 base32, lowercase, no padding. */
export function toBase32(b: Uint8Array): string {
  let bits = 0;
  let val = 0;
  let s = '';
  for (const x of b) {
    val = (val << 8) | x;
    bits += 8;
    while (bits >= 5) {
      s += B32[(val >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) s += B32[(val << (5 - bits)) & 31];
  return s;
}

export function concatBytes(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

export function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

export const utf8 = (s: string): Uint8Array => new TextEncoder().encode(s);
export const utf8Len = (s: string): number => new TextEncoder().encode(s).length;

