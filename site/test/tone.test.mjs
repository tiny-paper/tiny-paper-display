// SPDX-License-Identifier: MIT
// The tone curve (site/src/display/tone.ts, in the bundle sketch.js): a pure function of level, depth and step.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { TONE_MAX, TONE_MIN, toneGamma, toneLevel, toneTable } from '../sketch.js';

test('neutral is the identity at every depth', () => {
  for (const max of [1, 3, 15]) assert.deepEqual([...toneTable(max, 0)], Array.from({ length: max + 1 }, (_, v) => v));
  assert.equal(toneGamma(0), 1);
});

test('16 levels in, 16 levels out, the ends stay black and white, every step', () => {
  for (let step = TONE_MIN; step <= TONE_MAX; step++) {
    const t = toneTable(15, step);
    assert.equal(t.length, 16);
    assert.equal(t[0], 0, `step ${step}: black stays black`);
    assert.equal(t[15], 15, `step ${step}: white stays white`);
    assert.ok([...t].every((v) => Number.isInteger(v) && v >= 0 && v <= 15));
  }
});

test('monotonic: a lighter swatch is never sent darker; and a step up never sends a level lighter', () => {
  for (let step = TONE_MIN; step <= TONE_MAX; step++) {
    const t = toneTable(15, step);
    for (let v = 1; v < 16; v++) assert.ok(t[v] >= t[v - 1], `step ${step}: level ${v}`);
  }
  for (let step = TONE_MIN; step < TONE_MAX; step++) {
    const a = toneTable(15, step), b = toneTable(15, step + 1);
    for (let v = 0; v < 16; v++) assert.ok(b[v] <= a[v], `step ${step + 1} is not lighter than ${step} at level ${v}`);
  }
  assert.ok(toneLevel(8, 15, 3) < 8 && toneLevel(8, 15, -3) > 8);
});

test('a level outside the depth is refused, a step outside the range is clamped, a non-number is neutral', () => {
  assert.throws(() => toneLevel(16, 15, 0), /outside the depth/);
  assert.throws(() => toneLevel(-1, 15, 0), /outside the depth/);
  assert.equal(toneGamma(99), toneGamma(TONE_MAX));
  assert.equal(toneGamma(NaN), 1);
});
