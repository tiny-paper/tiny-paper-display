// Three test pictures made in the browser from numbers alone (no files from others, no random numbers, no fonts): the same
// bytes every time, on every machine. Each is made at the size it is asked for, normally the panel's, so it is shown 1:1.
import { drawText, textWidth, GLYPH_H } from './font.ts';
import type { Source } from './prepare.ts';

export const BUILTINS = ['gradient', 'scene', 'lines'] as const;
export type Builtin = typeof BUILTINS[number];
export const BUILTIN_NAMES: Record<Builtin, string> = { gradient: 'Gray gradient and step wedge', scene: 'Photo-like scene', lines: 'Line drawing' };

const smooth = (a: number, b: number, v: number): number => { const t = Math.min(1, Math.max(0, (v - a) / (b - a))); return t * t * (3 - 2 * t); };
const mix = (a: number, b: number, t: number): number => a + (b - a) * t;
/** A fixed pseudo-random value 0..1 for a pixel: integer hashing, no state. */
export function noise(x: number, y: number, seed = 0): number {
  let h = Math.imul(x, 374761393) ^ Math.imul(y, 668265263) ^ Math.imul(seed + 1, 2147483647);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

class Canvas {
  readonly rgba: Uint8ClampedArray;
  constructor(readonly w: number, readonly h: number, bg: number) { this.rgba = new Uint8ClampedArray(w * h * 4).fill(255); if (bg !== 255) for (let i = 0; i < w * h; i++) this.rgba.fill(bg, i * 4, i * 4 + 3); }
  set(x: number, y: number, r: number, g = r, b = r): void {
    if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
    const i = (y * this.w + x) * 4;
    this.rgba[i] = r; this.rgba[i + 1] = g; this.rgba[i + 2] = b;
  }
  rect(x: number, y: number, w: number, h: number, v: number): void { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) this.set(x + i, y + j, v); }
  text(s: string, x: number, y: number, scale: number, v: number): void { drawText(s, x, y, scale, (px, py) => this.set(px, py, v)); }
  /** A line `t` pixels thick (a square stamp), black unless `v` says otherwise. */
  line(x0: number, y0: number, x1: number, y1: number, t = 1, v = 0): void {
    const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0), 1);
    for (let i = 0; i <= n; i++) {
      const x = Math.round(x0 + (x1 - x0) * i / n), y = Math.round(y0 + (y1 - y0) * i / n);
      this.rect(x - Math.floor((t - 1) / 2), y - Math.floor((t - 1) / 2), t, t, v);
    }
  }
  circle(cx: number, cy: number, r: number, t = 1, v = 0): void {
    const n = Math.max(16, Math.ceil(r * 8));
    for (let i = 0; i < n; i++) { const a = i * 2 * Math.PI / n; this.rect(Math.round(cx + r * Math.cos(a)), Math.round(cy + r * Math.sin(a)), t, t, v); }
  }
}

/** Smooth ramp, a step wedge of `levels` equal bands, a ramp the other way, a 1 pixel checker, labels. */
function gradient(w: number, h: number, levels: number): Source {
  const c = new Canvas(w, h, 255);
  const ramp = Math.floor(h * 0.4), wedge = Math.floor(h * 0.3), unit = Math.max(1, Math.round(Math.min(w, h) / 200));
  for (let y = 0; y < ramp; y++) for (let x = 0; x < w; x++) c.set(x, y, Math.round(255 * x / Math.max(1, w - 1)));
  for (let i = 0; i < levels; i++) {
    const x0 = Math.floor(i * w / levels), x1 = Math.floor((i + 1) * w / levels);
    c.rect(x0, ramp, x1 - x0, wedge, Math.round(255 * i / Math.max(1, levels - 1)));
    if (levels <= 16 && x1 - x0 > 8 * unit) c.text(String(i), x0 + 2 * unit, ramp + wedge - 9 * unit, unit, i * 2 < levels ? 255 : 0);
  }
  const y2 = ramp + wedge, y3 = Math.floor(h * 0.9);
  for (let y = y2; y < y3; y++) for (let x = 0; x < w; x++) c.set(x, y, Math.round(255 * (1 - x / Math.max(1, w - 1))));
  for (let y = y3; y < h; y++) for (let x = 0; x < w; x++) c.set(x, y, x < w / 2 ? ((x + y) & 1 ? 255 : 0) : 128);
  c.text('GRAY GRADIENT', 2 * unit, 2 * unit, 2 * unit, 255);
  return { w, h, rgba: c.rgba };
}

/** Soft sky and sun, hills with fine texture, a face-like oval, a plaque with text at three sizes. */
function scene(w: number, h: number): Source {
  const c = new Canvas(w, h, 255), m = Math.min(w, h);
  const fx = w * 0.3, fy = h * 0.58, rx = m * 0.17, ry = m * 0.23;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const u = x / w, v = y / h;
    let r = mix(70, 190, v * 1.2), g = mix(120, 215, v * 1.2), b = mix(205, 245, v);
    const d = Math.hypot(x - w * 0.78, y - h * 0.2) / m;
    const sun = smooth(0.1, 0.06, d), glow = smooth(0.45, 0.06, d) * 0.45;
    r = mix(r, 255, glow); g = mix(g, 245, glow); b = mix(b, 200, glow * 0.7);
    r = mix(r, 255, sun); g = mix(g, 250, sun); b = mix(b, 215, sun);
    const far = 0.55 + 0.05 * Math.sin(u * 7 + 1) + 0.02 * Math.sin(u * 19), near = 0.7 + 0.05 * Math.sin(u * 4 + 3) + 0.015 * Math.sin(u * 23);
    const a1 = smooth(far + 0.006, far - 0.006, v), a2 = smooth(near + 0.006, near - 0.006, v);
    r = mix(r, 95, a1); g = mix(g, 120, a1); b = mix(b, 135, a1);
    const grass = (noise(x, y, 1) - 0.5) * 46 + (noise(Math.floor(x / 3), y, 2) - 0.5) * 30;
    r = mix(r, 70 + grass, a2); g = mix(g, 135 + grass, a2); b = mix(b, 55 + grass * 0.6, a2);
    const e = ((x - fx) / rx) ** 2 + ((y - fy) / ry) ** 2, face = smooth(1.06, 0.94, e);
    if (face > 0) {
      const shade = 1 - 0.35 * e + 0.12 * ((fx - x) / rx);
      const hair = smooth(-0.1, 0.1, (fy - ry * 0.45 - y) / ry) * face;
      r = mix(r, 235 * shade, face); g = mix(g, 185 * shade, face); b = mix(b, 150 * shade, face);
      r = mix(r, 60, hair); g = mix(g, 40, hair); b = mix(b, 30, hair);
      for (const s of [-1, 1]) {
        const ee = ((x - fx - s * rx * 0.4) / (rx * 0.16)) ** 2 + ((y - fy + ry * 0.12) / (ry * 0.07)) ** 2, eye = smooth(1.1, 0.8, ee);
        r = mix(r, 40, eye); g = mix(g, 35, eye); b = mix(b, 35, eye);
      }
      const mo = ((x - fx) / (rx * 0.4)) ** 2 + ((y - fy - ry * 0.45) / (ry * 0.07)) ** 2, mouth = smooth(1.1, 0.8, mo);
      r = mix(r, 150, mouth); g = mix(g, 50, mouth); b = mix(b, 60, mouth);
    }
    c.set(x, y, r, g, b);
  }
  const k = Math.max(1, Math.round(m / 330)), lines: [string, number][] = [['Small text 1234567890', k], ['MEDIUM TEXT', 2 * k], ['LARGE', 4 * k]];
  const pw = Math.max(...lines.map(([s, z]) => textWidth(s, z))) + 8 * k, ph = lines.reduce((n, [, z]) => n + (GLYPH_H + 2) * z, 0) + 6 * k;
  const px = Math.max(0, w - pw - 4 * k), py = Math.max(0, h - ph - 4 * k);
  c.rect(px, py, pw, ph, 250);
  let ty = py + 3 * k;
  for (const [s, z] of lines) { c.text(s, px + 4 * k, ty, z, 20); ty += (GLYPH_H + 2) * z; }
  return { w, h, rgba: c.rgba };
}

/** Black on white: thin lines at 1, 2 and 3 pixels, line pairs closer and closer, circles, a fan, small text. */
function lines(w: number, h: number): Source {
  const c = new Canvas(w, h, 255), m = Math.min(w, h), u = Math.max(1, Math.round(m / 300));
  c.rect(0, 0, w, 1, 0); c.rect(0, h - 1, w, 1, 0); c.rect(0, 0, 1, h, 0); c.rect(w - 1, 0, 1, h, 0);
  for (let i = 0; i < 3; i++) c.line(w * 0.05, h * (0.1 + 0.05 * i), w * 0.45, h * (0.1 + 0.05 * i) + h * 0.05, i + 1);
  for (let i = 0; i < 5; i++) { const gap = i + 1, x0 = Math.round(w * 0.55); for (let j = 0; j < 12; j++) c.line(x0 + j * 2 * gap, Math.round(h * (0.08 + 0.05 * i)), x0 + j * 2 * gap, Math.round(h * (0.08 + 0.05 * i)) + Math.round(h * 0.04)); }
  for (let i = 0; i <= 12; i++) c.line(w * 0.05, h * 0.9, w * 0.45, h * (0.35 + 0.05 * i));
  for (let r = 1; r <= 6; r++) c.circle(w * 0.75, h * 0.55, r * m * 0.05, 1);
  const bx = w * 0.08, by = h * 0.5, bw = w * 0.25, bh = h * 0.25;
  c.line(bx, by + bh * 0.3, bx + bw / 2, by, 2); c.line(bx + bw / 2, by, bx + bw, by + bh * 0.3, 2);
  c.line(bx + 4, by + bh * 0.3, bx + 4, by + bh, 2); c.line(bx + bw - 4, by + bh * 0.3, bx + bw - 4, by + bh, 2); c.line(bx + 4, by + bh, bx + bw - 4, by + bh, 2);
  c.rect(Math.round(bx + bw * 0.42), Math.round(by + bh * 0.6), Math.round(bw * 0.16), Math.round(bh * 0.4), 0);
  c.text('LINE DRAWING 1 PX AND 2 PX', Math.round(w * 0.05), Math.round(h * 0.93), u, 0);
  c.text('SMALL TEXT FOR READING', Math.round(w * 0.55), Math.round(h * 0.84), u, 0);
  c.text('MEDIUM TEXT', Math.round(w * 0.55), Math.round(h * 0.88), 2 * u, 0);
  c.text('BIG', Math.round(w * 0.55), Math.round(h * 0.94), 3 * u, 0);
  return { w, h, rgba: c.rgba };
}

/** `levels`: how many gray levels the step wedge shows (the deepest depth of the device). */
export function builtinPicture(kind: Builtin, w: number, h: number, levels: number): Source {
  return kind === 'gradient' ? gradient(w, h, levels) : kind === 'scene' ? scene(w, h) : lines(w, h);
}
