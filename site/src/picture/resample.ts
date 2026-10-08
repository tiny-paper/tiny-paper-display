// Scaling one plane of numbers (a gray channel) to another size. Shrinking averages the source cells a new pixel covers
// (a box filter: every source pixel counts, so thin lines and small text do not drop out). Enlarging is bilinear at the
// pixel centres. A scale of exactly 1 with a whole-pixel start copies. Pure, so the preview and the send are the same pixels.
export interface SrcRect { x0: number; y0: number; x1: number; y1: number }
interface Taps { k0: number; w: number[] }

/** For each new index: the first source index and the weights (sum 1) of the source indexes from there. */
export function axisTaps(srcN: number, from: number, to: number, dstN: number): Taps[] {
  const scale = (to - from) / dstN;
  const out: Taps[] = [];
  for (let i = 0; i < dstN; i++) {
    if (scale > 1) {
      const a = from + i * scale, b = a + scale;
      const k0 = Math.max(0, Math.floor(a)), k1 = Math.min(srcN - 1, Math.ceil(b) - 1);
      const w: number[] = [];
      for (let k = k0; k <= k1; k++) w.push(Math.max(0, Math.min(b, k + 1) - Math.max(a, k)));
      const sum = w.reduce((s, v) => s + v, 0) || 1;
      out.push({ k0, w: w.map((v) => v / sum) });
    } else {
      const p = from + (i + 0.5) * scale - 0.5, k = Math.floor(p), f = p - k;
      const a = Math.min(srcN - 1, Math.max(0, k)), b = Math.min(srcN - 1, Math.max(0, k + 1));
      out.push(a === b ? { k0: a, w: [1] } : { k0: a, w: [1 - f, f] });
    }
  }
  return out;
}

/** `rect` (in source pixels, may be fractional) of the `sw` x `sh` plane `src`, scaled to `dw` x `dh`. */
export function resamplePlane(src: Float32Array, sw: number, sh: number, rect: SrcRect, dw: number, dh: number): Float32Array {
  const tx = axisTaps(sw, rect.x0, rect.x1, dw), ty = axisTaps(sh, rect.y0, rect.y1, dh);
  const r0 = Math.min(...ty.map((t) => t.k0)), r1 = Math.max(...ty.map((t) => t.k0 + t.w.length - 1));
  const mid = new Float32Array(dw * (r1 - r0 + 1));
  for (let y = r0; y <= r1; y++) {
    const row = y * sw, o = (y - r0) * dw;
    for (let x = 0; x < dw; x++) {
      const t = tx[x]!;
      let s = 0;
      for (let j = 0; j < t.w.length; j++) s += src[row + t.k0 + j]! * t.w[j]!;
      mid[o + x] = s;
    }
  }
  const out = new Float32Array(dw * dh);
  for (let y = 0; y < dh; y++) {
    const t = ty[y]!;
    for (let x = 0; x < dw; x++) {
      let s = 0;
      for (let j = 0; j < t.w.length; j++) s += mid[(t.k0 + j - r0) * dw + x]! * t.w[j]!;
      out[y * dw + x] = s;
    }
  }
  return out;
}
