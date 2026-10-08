// The tone curve of the Sketch page, a measuring aid: the level SENT for a swatch of the drawing. The page shows a
// swatch as a screen gray (level n of max as n/max of white); a panel may show that level lighter or darker. The curve
// is a gamma on the level's share of white, so a person can find the setting at which the panel matches the swatch.
export const TONE_MIN = -6;
export const TONE_MAX = 6;
const BASE = 1.2;

/** The step as an exponent: 0 is neutral (1), above 0 sends darker levels, below 0 lighter ones. Clamped to the range. */
export function toneGamma(step: number): number {
  const s = Number.isFinite(step) ? Math.min(TONE_MAX, Math.max(TONE_MIN, Math.round(step))) : 0;
  return BASE ** s;
}

/** The level sent for `level` (0 black .. max white): max * (level / max) ^ gamma, rounded. Neutral: the level itself. */
export function toneLevel(level: number, max: number, step: number): number {
  if (!Number.isInteger(level) || !Number.isInteger(max) || max < 1 || level < 0 || level > max) throw new Error('The level is outside the depth');
  if (toneGamma(step) === 1) return level;
  return Math.round(max * (level / max) ** toneGamma(step));
}

/** The table of every level 0..max (16 in, 16 out at 4 bit). */
export function toneTable(max: number, step: number): Uint8Array {
  return Uint8Array.from({ length: max + 1 }, (_, v) => toneLevel(v, max, step));
}
