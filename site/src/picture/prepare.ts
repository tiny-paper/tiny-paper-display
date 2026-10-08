// A picture made ready for a panel: rotate, scale to fit, put on the panel, then reduce to the panel's levels.
// Luminance is Y = 0.299 R + 0.587 G + 0.114 B on the stored (gamma) values, not on linear light; the levels of a gray
// depth are evenly spaced in those same values, as on the Sketch page. Scaling is on the luminance (or each colour
// channel on the PicPak), dithering comes after scaling. A transparent pixel counts as the border colour.
import { packRows } from '../protocol/album.ts';
import { DISPLAY_PALETTE, packedPng } from '../display/png.ts';
import { resamplePlane, type SrcRect } from './resample.ts';

export interface Source { w: number; h: number; rgba: ArrayLike<number> }
export type Fit = 'fit' | 'fill' | 'stretch';
export type Border = 'white' | 'black';
export type Rotate = 0 | 90 | 180 | 270 | 'auto';
export type Dither = 'none' | 'floyd' | 'ordered';
export interface Panel { w: number; h: number }
export interface Settings { fit: Fit; border: Border; rotate: Rotate; depth: number; color: boolean; dither: Dither; threshold: number }
export const DEFAULT_SETTINGS: Settings = { fit: 'fit', border: 'white', rotate: 'auto', depth: 1, color: false, dither: 'floyd', threshold: 128 };
export const MAX_SOURCE_PIXELS = 48_000_000;

/** The source turned clockwise by 0, 90, 180 or 270 degrees. */
export function rotateSource(s: Source, deg: 0 | 90 | 180 | 270): Source {
  if (deg === 0) return s;
  const w = deg === 180 ? s.w : s.h, h = deg === 180 ? s.h : s.w;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < s.h; y++) for (let x = 0; x < s.w; x++) {
    const nx = deg === 90 ? s.h - 1 - y : deg === 180 ? s.w - 1 - x : y;
    const ny = deg === 90 ? x : deg === 180 ? s.h - 1 - y : s.w - 1 - x;
    const a = (y * s.w + x) * 4, b = (ny * w + nx) * 4;
    for (let c = 0; c < 4; c++) out[b + c] = s.rgba[a + c]!;
  }
  return { w, h, rgba: out };
}

/** Auto: turn the picture a quarter when it then fills more of the panel (a larger scale when the whole picture must fit). */
export function pickRotation(src: Panel, panel: Panel, rotate: Rotate): 0 | 90 | 180 | 270 {
  if (rotate !== 'auto') return rotate;
  const s0 = Math.min(panel.w / src.w, panel.h / src.h), s90 = Math.min(panel.w / src.h, panel.h / src.w);
  return s90 > s0 ? 90 : 0;
}

/** The picture on the panel, still in 0..255 values: one plane (luminance) or three (red, green, blue). */
export interface Composed { w: number; h: number; planes: Float32Array[]; rotation: 0 | 90 | 180 | 270; box: { x: number; y: number; w: number; h: number } }

export function compose(source: Source, panel: Panel, s: Pick<Settings, 'fit' | 'border' | 'rotate' | 'color'>): Composed {
  if (source.w < 1 || source.h < 1 || source.w * source.h > MAX_SOURCE_PIXELS) throw new Error('The picture is too large or empty');
  const rotation = pickRotation(source, panel, s.rotate);
  const src = rotateSource(source, rotation);
  const edge = s.border === 'white' ? 255 : 0;
  const n = s.color ? 3 : 1;
  const raw = Array.from({ length: n }, () => new Float32Array(src.w * src.h));
  for (let i = 0; i < src.w * src.h; i++) {
    const a = src.rgba[i * 4 + 3]! / 255;
    const ch = [src.rgba[i * 4]!, src.rgba[i * 4 + 1]!, src.rgba[i * 4 + 2]!].map((v) => v * a + edge * (1 - a));
    if (n === 1) raw[0]![i] = 0.299 * ch[0]! + 0.587 * ch[1]! + 0.114 * ch[2]!;
    else for (let c = 0; c < 3; c++) raw[c]![i] = ch[c]!;
  }
  let rect: SrcRect = { x0: 0, y0: 0, x1: src.w, y1: src.h };
  let box = { x: 0, y: 0, w: panel.w, h: panel.h };
  if (s.fit === 'fit') {
    const k = Math.min(panel.w / src.w, panel.h / src.h);
    const w = Math.min(panel.w, Math.max(1, Math.round(src.w * k))), h = Math.min(panel.h, Math.max(1, Math.round(src.h * k)));
    box = { x: Math.floor((panel.w - w) / 2), y: Math.floor((panel.h - h) / 2), w, h };
  } else if (s.fit === 'fill') {
    const k = Math.max(panel.w / src.w, panel.h / src.h), cw = panel.w / k, ch = panel.h / k;
    rect = { x0: (src.w - cw) / 2, y0: (src.h - ch) / 2, x1: (src.w + cw) / 2, y1: (src.h + ch) / 2 };
  }
  const planes = raw.map((p) => {
    const small = resamplePlane(p, src.w, src.h, rect, box.w, box.h);
    if (box.w === panel.w && box.h === panel.h) return small;
    const full = new Float32Array(panel.w * panel.h).fill(edge);
    for (let y = 0; y < box.h; y++) full.set(small.subarray(y * box.w, (y + 1) * box.w), (box.y + y) * panel.w + box.x);
    return full;
  });
  return { w: panel.w, h: panel.h, planes, rotation, box };
}

const BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];
const clamp = (v: number, lo: number, hi: number): number => (v < lo ? lo : v > hi ? hi : v);

function grayLevels(plane: Float32Array, w: number, h: number, depth: number, dither: Dither, threshold: number): Uint8Array {
  const n1 = (1 << depth) - 1, out = new Uint8Array(w * h);
  if (dither === 'none') {
    for (let i = 0; i < out.length; i++) out[i] = depth === 1 ? (plane[i]! >= threshold ? 1 : 0) : clamp(Math.round(plane[i]! * n1 / 255), 0, n1);
  } else if (dither === 'ordered') {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const t = (BAYER4[y % 4]![x % 4]! + 0.5) / 16;
      out[y * w + x] = clamp(Math.floor(plane[y * w + x]! * n1 / 255 + t), 0, n1);
    }
  } else {
    const buf = Float32Array.from(plane);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, old = buf[i]!, q = clamp(Math.round(old * n1 / 255), 0, n1);
      out[i] = q;
      const err = old - q * 255 / n1;
      if (x + 1 < w) buf[i + 1]! += err * 7 / 16;
      if (y + 1 < h) {
        if (x > 0) buf[i + w - 1]! += err * 3 / 16;
        buf[i + w]! += err * 5 / 16;
        if (x + 1 < w) buf[i + w + 1]! += err / 16;
      }
    }
  }
  return out;
}

function nearestColor(r: number, g: number, b: number): number {
  let best = 0, bd = Infinity;
  DISPLAY_PALETTE.forEach((p, i) => { const d = (r - p[0]) ** 2 + (g - p[1]) ** 2 + (b - p[2]) ** 2; if (d < bd) { bd = d; best = i; } });
  return best;
}

function colorLevels(planes: Float32Array[], w: number, h: number, dither: Dither): Uint8Array {
  const out = new Uint8Array(w * h);
  if (dither === 'floyd') {
    const buf = planes.map((p) => Float32Array.from(p));
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, q = nearestColor(buf[0]![i]!, buf[1]![i]!, buf[2]![i]!);
      out[i] = q;
      for (let c = 0; c < 3; c++) {
        const err = buf[c]![i]! - DISPLAY_PALETTE[q]![c]!, p = buf[c]!;
        if (x + 1 < w) p[i + 1]! += err * 7 / 16;
        if (y + 1 < h) { if (x > 0) p[i + w - 1]! += err * 3 / 16; p[i + w]! += err * 5 / 16; if (x + 1 < w) p[i + w + 1]! += err / 16; }
      }
    }
  } else {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, bias = dither === 'ordered' ? ((BAYER4[y % 4]![x % 4]! + 0.5) / 16 - 0.5) * 128 : 0;
      out[i] = nearestColor(planes[0]![i]! + bias, planes[1]![i]! + bias, planes[2]![i]! + bias);
    }
  }
  return out;
}

/** One value per panel pixel: 0 to 2^depth - 1 (0 black), or on the colour panel 0 black, 1 red, 2 yellow, 3 white. */
export function quantise(c: Composed, s: Pick<Settings, 'depth' | 'color' | 'dither' | 'threshold'>): Uint8Array {
  return s.color ? colorLevels(c.planes, c.w, c.h, s.dither) : grayLevels(c.planes[0]!, c.w, c.h, s.depth, s.dither, s.threshold);
}

/** The pixels the panel will show, as RGBA for the preview. */
export function levelsToRgba(levels: Uint8Array, depth: number, color: boolean): Uint8ClampedArray {
  const out = new Uint8ClampedArray(levels.length * 4), max = (1 << depth) - 1;
  for (let i = 0; i < levels.length; i++) {
    const v = levels[i]!, rgb = color ? DISPLAY_PALETTE[v]! : [Math.round(v * 255 / max), Math.round(v * 255 / max), Math.round(v * 255 / max)];
    out.set([rgb[0]!, rgb[1]!, rgb[2]!, 255], i * 4);
  }
  return out;
}

export interface Prepared { w: number; h: number; depth: number; color: boolean; levels: Uint8Array; rotation: 0 | 90 | 180 | 270; settings: Settings }
export function preparePicture(source: Source, panel: Panel, s: Settings, composed = compose(source, panel, s)): Prepared {
  return { w: panel.w, h: panel.h, depth: s.color ? 2 : s.depth, color: s.color, levels: quantise(composed, s), rotation: composed.rotation, settings: s };
}
/** The PNG the device takes: exactly the panel's size, grayscale at the depth, or indexed with the panel palette. */
export function preparedPng(p: Prepared): Promise<Uint8Array> {
  return packedPng(packRows(p.levels, p.w, p.h, p.depth), p.w, p.h, p.depth, p.color);
}
