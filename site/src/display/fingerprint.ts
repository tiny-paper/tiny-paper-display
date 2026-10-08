// The duplicate-picture fingerprint of docs/spec/usb-display.md ("Fingerprint and duplicate pictures"): FNV-1a, 64 bits,
// over width and height (u16 BE), the depth, the colour flag and the packed rows (first pixel in the high bits, unused low
// bits of a row zero). The device tells in hello which picture it keeps; the page compares it with its own canvas.
const OFFSET = [0x84222325, 0xcbf29ce4]; // low, high 32 bits of cbf29ce484222325

/** `levels`: one value per pixel, rows top to bottom (w * h of them). 16 lowercase hex digits. */
export function fingerprintOf(levels: Uint8Array, w: number, h: number, depth: number, color = false): string {
  let lo = OFFSET[0]!, hi = OFFSET[1]!;
  const feed = (b: number): void => {
    lo = (lo ^ b) >>> 0;
    // x * 0x100000001b3 mod 2^64 = x * 0x1b3 + (x << 40), in 32-bit halves
    const m = lo * 0x1b3;
    const carry = Math.floor(m / 4294967296);
    const nlo = m % 4294967296;
    hi = (hi * 0x1b3 + carry + lo * 256) % 4294967296;
    lo = nlo;
  };
  for (const b of [w >> 8 & 255, w & 255, h >> 8 & 255, h & 255, depth, color ? 1 : 0]) feed(b);
  for (let y = 0; y < h; y++) {
    let acc = 0, n = 0;
    for (let x = 0; x < w; x++) {
      acc = (acc << depth) | levels[y * w + x]!;
      n += depth;
      if (n === 8) { feed(acc); acc = 0; n = 0; }
    }
    if (n) feed((acc << (8 - n)) & 255);
  }
  return `${hi.toString(16).padStart(8, '0')}${lo.toString(16).padStart(8, '0')}`;
}
