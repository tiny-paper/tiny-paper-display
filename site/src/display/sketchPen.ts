// Sketch on a USB display: the pen. Pointer positions in panel pixels become strokes of the drawing (sketchModel.ts)
// and tell the sender (sketchSender.ts) when to send; the page only maps its pointer events to these calls.
import { sketchBrushes } from './sketchModel.ts';
import type { SketchSender } from './sketchSender.ts';

export interface PenPoint { x: number; y: number }
export const BRUSH_NAMES = ['Small', 'Medium', 'Large'] as const;

export class SketchPen {
  /** Index into `brushes`. */
  size = 1;
  /** The swatch value drawn with while the eraser is off. */
  color = 0;
  eraser = false;
  readonly brushes: [number, number, number];
  private last: { id: number; x: number; y: number } | null = null;

  constructor(readonly sender: SketchSender) { this.brushes = sketchBrushes(sender.model.w); }
  get canUndo(): boolean { return this.sender.model.canUndo; }
  private get value(): number { return this.eraser ? this.sender.ink.paper : this.color; }
  private get width(): number { return this.brushes[this.size] ?? this.brushes[1]; }

  /** A pointer went down; false when another one is already drawing. */
  down(id: number, p: PenPoint): boolean {
    if (this.last) return false;
    this.last = { id, ...p };
    this.sender.model.begin(); this.sender.begin();
    this.sender.model.line(p.x, p.y, p.x, p.y, this.width, this.value);
    this.sender.changed();
    return true;
  }
  /** The drawing pointer moved through these points. */
  move(id: number, points: readonly PenPoint[]): void {
    if (this.last?.id !== id) return;
    for (const p of points) {
      this.sender.model.line(this.last.x, this.last.y, p.x, p.y, this.width, this.value);
      this.last = { id, ...p };
    }
    this.sender.changed();
  }
  /** The drawing pointer lifted (or was cancelled); false for any other pointer. */
  up(id: number): boolean {
    if (this.last?.id !== id) return false;
    this.last = null;
    this.sender.model.end(); this.sender.lift();
    return true;
  }
  clear(): void { this.sender.model.clear(); this.sender.lift(); }
  undo(): void { if (this.sender.model.undo()) this.sender.lift(); }
}
