// Sketch on a USB display (docs/spec/usb-display.md "Area and refresh mode"): the drawing itself. One value per panel
// pixel, strokes as round stamps, the rectangle changed since the last send (x and width in steps of 8), undo.
import { DISPLAY_PALETTE } from './png.ts';
import type { DisplayHello } from './client.ts';

export interface Rect { x: number; y: number; w: number; h: number }
export interface Swatch { value: number; name: string; rgb: readonly [number, number, number] }
/** What the sketch draws with: the depth sent, the colours the panel shows at it, and whether strokes go out as partial updates. */
export interface SketchInk { depth: number; color: boolean; partial: boolean; paper: number; swatches: Swatch[]; note: string }
export const UNDO_STEPS = 10;

/** The depth the device updates partially at decides the colours: a rectangle must have a depth in `partial_depths`. */
export function sketchInk(hello: DisplayHello): SketchInk {
  const partial = hello.modes.includes('partial') && hello.partial_depths.length > 0;
  const color = !partial && !!hello.colors;
  const depth = partial ? Math.max(...hello.partial_depths) : color ? 2 : Math.max(...hello.depths);
  const max = (1 << depth) - 1;
  const swatches: Swatch[] = [];
  if (color) {
    const value: Record<string, number> = { black: 0, red: 1, yellow: 2, white: 3 };
    for (const name of hello.colors!) swatches.push({ value: value[name]!, name, rgb: DISPLAY_PALETTE[value[name]!]! as Swatch['rgb'] });
  } else for (let v = 0; v <= max; v++) {
    const g = Math.round(v * 255 / max);
    swatches.push({ value: v, name: v === 0 ? 'black' : v === max ? 'white' : `gray ${v} of ${max}`, rgb: [g, g, g] });
  }
  const more = hello.colors ? 'colours' : Math.max(...hello.depths) > depth ? 'grays' : '';
  const note = !partial ? 'This panel has no partial refresh: the drawing goes out only with the Full refresh button.'
    : more ? `Partial updates on this panel cannot show ${depth === 1 ? more : 'more grays'}, so the sketch is ${depth === 1 ? 'black and white' : `${max + 1} grays`}.` : '';
  return { depth, color, partial, paper: max, swatches, note };
}

/** Three brush widths in panel pixels, for a panel of this width (4, 12 and 32 on the 1872 wide X). */
export function sketchBrushes(w: number): [number, number, number] {
  return [Math.max(1, Math.round(w / 470)), Math.max(2, Math.round(w / 156)), Math.max(3, Math.round(w / 58))];
}

/** The smallest rectangle with x and width multiples of 8 that holds `r`; the whole panel when that does not fit (a width not in steps of 8). */
export function alignRect(r: Rect, w: number, h: number): Rect {
  const x = Math.floor(r.x / 8) * 8, end = Math.ceil((r.x + r.w) / 8) * 8;
  return end > w ? { x: 0, y: 0, w, h } : { x, y: r.y, w: end - x, h: r.h };
}

type Box = [number, number, number, number]; // x0, y0, x1, y1 (exclusive)
const grow = (b: Box | null, x0: number, y0: number, x1: number, y1: number): Box =>
  b ? [Math.min(b[0], x0), Math.min(b[1], y0), Math.max(b[2], x1), Math.max(b[3], y1)] : [x0, y0, x1, y1];
const rectOf = (b: Box): Rect => ({ x: b[0], y: b[1], w: b[2] - b[0], h: b[3] - b[1] });

export class SketchModel {
  readonly px: Uint8Array;
  /** Called with every rectangle whose pixels changed (the page repaints it). */
  onPaint: (r: Rect) => void = () => {};
  private dirty: Box | null = null;
  private stroke: Box | null = null;
  private before: Uint8Array | null = null;
  private scratch: Uint8Array;
  private undos: { rect: Rect; px: Uint8Array }[] = [];

  constructor(readonly w: number, readonly h: number, readonly paper: number) {
    if (!Number.isInteger(w) || !Number.isInteger(h) || w < 1 || h < 1 || w * h > 4_194_304) throw new Error('The panel size is invalid');
    this.px = new Uint8Array(w * h).fill(paper);
    this.scratch = new Uint8Array(w * h);
  }
  get canUndo(): boolean { return this.undos.length > 0; }
  get hasDirty(): boolean { return !!this.dirty; }
  get whole(): Rect { return { x: 0, y: 0, w: this.w, h: this.h }; }

  /** Start one undo step: a stroke, or a clear. */
  begin(): void {
    this.scratch.set(this.px);
    this.before = this.scratch;
    this.stroke = null;
  }
  /** A round brush `size` pixels wide dragged from one point to the next (the same point twice: a dot). */
  line(x0: number, y0: number, x1: number, y1: number, size: number, value: number): void {
    if (![x0, y0, x1, y1].every(Number.isFinite)) return;
    const r = Math.max(0, (size - 1) / 2), reach = Math.ceil(r);
    const steps = Math.min(8192, Math.ceil(Math.hypot(x1 - x0, y1 - y0) / Math.max(1, r / 2)));
    let box: Box | null = null;
    for (let i = 0; i <= steps; i++) {
      const cx = Math.round(steps ? x0 + (x1 - x0) * i / steps : x0), cy = Math.round(steps ? y0 + (y1 - y0) * i / steps : y0);
      for (let dy = -reach; dy <= reach; dy++) {
        const y = cy + dy;
        if (y < 0 || y >= this.h) continue;
        const half = Math.floor(Math.sqrt(r * r - dy * dy + 0.25));
        if (Number.isNaN(half)) continue;
        for (let x = Math.max(0, cx - half), end = Math.min(this.w - 1, cx + half); x <= end; x++) {
          if (this.px[y * this.w + x] === value) continue;
          this.px[y * this.w + x] = value;
          box = grow(box, x, y, x + 1, y + 1);
        }
      }
    }
    if (box) this.changed(box);
  }
  /** Close the step; it is kept for undo only when a pixel changed. */
  end(): void {
    const b = this.stroke, before = this.before;
    this.before = this.stroke = null;
    if (!b || !before) return;
    const rect = rectOf(b);
    this.undos.push({ rect, px: this.copy(before, rect) });
    if (this.undos.length > UNDO_STEPS) this.undos.shift();
  }
  clear(): void {
    this.begin();
    let box: Box | null = null;
    for (let y = 0; y < this.h; y++) for (let x = 0; x < this.w; x++) if (this.px[y * this.w + x] !== this.paper) box = grow(box, x, y, x + 1, y + 1);
    if (box) { this.px.fill(this.paper); this.changed(box); }
    this.end();
  }
  undo(): boolean {
    const u = this.undos.pop();
    if (!u) return false;
    for (let y = 0; y < u.rect.h; y++) this.px.set(u.px.subarray(y * u.rect.w, (y + 1) * u.rect.w), (u.rect.y + y) * this.w + u.rect.x);
    this.before = this.stroke = null;
    this.changed([u.rect.x, u.rect.y, u.rect.x + u.rect.w, u.rect.y + u.rect.h]);
    return true;
  }
  /** The rectangle changed since the last call, aligned for a `show` with x and y; null when nothing changed. */
  takeDirty(): Rect | null {
    const d = this.dirty;
    this.dirty = null;
    return d ? alignRect(rectOf(d), this.w, this.h) : null;
  }
  /** Put a rectangle back (its send failed). */
  markDirty(r: Rect): void { this.dirty = grow(this.dirty, r.x, r.y, r.x + r.w, r.y + r.h); }
  /** The pixel values of a rectangle, rows top to bottom. */
  values(r: Rect): Uint8Array { return this.copy(this.px, r); }
  /** The rectangle as the panel shows it. */
  rgba(r: Rect, ink: SketchInk): Uint8ClampedArray<ArrayBuffer> {
    const out = new Uint8ClampedArray(r.w * r.h * 4), rgb = new Map(ink.swatches.map((s) => [s.value, s.rgb]));
    for (let y = 0; y < r.h; y++) for (let x = 0; x < r.w; x++) {
      const c = rgb.get(this.px[(r.y + y) * this.w + r.x + x]!) ?? [255, 255, 255];
      out.set([c[0], c[1], c[2], 255], (y * r.w + x) * 4);
    }
    return out;
  }
  private copy(from: Uint8Array, r: Rect): Uint8Array {
    const out = new Uint8Array(r.w * r.h);
    for (let y = 0; y < r.h; y++) out.set(from.subarray((r.y + y) * this.w + r.x, (r.y + y) * this.w + r.x + r.w), y * r.w);
    return out;
  }
  private changed(b: Box): void {
    this.dirty = grow(this.dirty, ...b);
    if (this.before) this.stroke = grow(this.stroke, ...b);
    this.onPaint(rectOf(b));
  }
}
