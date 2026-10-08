// SPDX-License-Identifier: MIT
// The Picture page's pixels (site/src/picture, through the bundle picture.js): scaling, fit, rotation, dithering against
// hand-worked small cases, levels inside the device's set, the PNG's header, the built-in pictures, the Compare plan.
// Run: node --test site/test/*.test.mjs
import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  BUILTINS, DEFAULT_SETTINGS, builtinPicture, comparePlan, compose, depthChoices, partialLine, pickRotation, preparePicture, preparedPng,
  refreshChoices, reportText, resamplePlane, rotateSource,
} from '../picture.js';

/** A gray source from rows of 0..255 values. */
const gray = (rows) => ({ w: rows[0].length, h: rows.length, rgba: Uint8ClampedArray.from(rows.flat().flatMap((v) => [v, v, v, 255])) });
const set = (over) => ({ ...DEFAULT_SETTINGS, rotate: 0, ...over });
const levels = (src, panel, over) => Array.from(preparePicture(src, panel, set(over)).levels);
const rows = (list, w) => Array.from({ length: list.length / w }, (_, y) => list.slice(y * w, (y + 1) * w).join(''));
const HELLO = { proto: 1, project: 'display', fw: 't', board: 'trmnl_og', w: 800, h: 480, depths: [1, 2], partial_depths: [1], modes: ['full', 'partial', 'inverse'], full_every: 20, max_png: 131072, max_payload: 6144, chunk: 4096 };

test('fit: the whole picture centred with the border colour round it (4 x 2 on 4 x 4)', () => {
  const src = gray([[0, 0, 255, 255], [0, 0, 255, 255]]);
  assert.deepEqual(rows(levels(src, { w: 4, h: 4 }, { fit: 'fit', border: 'white' }), 4), ['1111', '0011', '0011', '1111']);
  assert.deepEqual(rows(levels(src, { w: 4, h: 4 }, { fit: 'fit', border: 'black' }), 4), ['0000', '0011', '0011', '0000']);
});

test('fill crops the middle and enlarges bilinearly (hand worked: 0, 63.75, 191.25, 255 at 4 levels is 0 1 2 3)', () => {
  const src = gray([[0, 0, 255, 255], [0, 0, 255, 255]]);
  assert.deepEqual(rows(levels(src, { w: 4, h: 4 }, { fit: 'fill', depth: 2, dither: 'none' }), 4), ['0123', '0123', '0123', '0123']);
});

test('stretch uses the whole panel without keeping the proportions', () => {
  const src = gray([[0, 0, 255, 255], [0, 0, 255, 255]]);
  assert.deepEqual(rows(levels(src, { w: 4, h: 4 }, { fit: 'stretch', dither: 'none' }), 4), ['0011', '0011', '0011', '0011']);
});

test('shrinking averages the cells a pixel covers: 0 100 200 40 to two pixels is 50 and 120 (3 and 7 of 15)', () => {
  assert.deepEqual(Array.from(resamplePlane(Float32Array.of(0, 100, 200, 40), 4, 1, { x0: 0, y0: 0, x1: 4, y1: 1 }, 2, 1)), [50, 120]);
  assert.deepEqual(Array.from(resamplePlane(Float32Array.of(0, 0, 90), 3, 1, { x0: 0, y0: 0, x1: 3, y1: 1 }, 1, 1)), [30], 'a box over 3 cells, not the one in the middle');
  assert.deepEqual(levels(gray([[0, 100, 200, 40]]), { w: 2, h: 1 }, { fit: 'stretch', depth: 4, dither: 'none' }), [3, 7]);
});

test('luminance is 0.299 R + 0.587 G + 0.114 B on the stored values; a transparent pixel is the border colour', () => {
  const src = { w: 3, h: 1, rgba: Uint8ClampedArray.of(255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255) };
  const c = compose(src, { w: 3, h: 1 }, set({ fit: 'stretch' }));
  assert.deepEqual(Array.from(c.planes[0], (v) => Math.round(v * 10) / 10), [76.2, 149.7, 29.1]);
  const clear = { w: 1, h: 1, rgba: Uint8ClampedArray.of(10, 10, 10, 0) };
  assert.deepEqual(levels(clear, { w: 1, h: 1 }, { border: 'white' }), [1]);
  assert.deepEqual(levels(clear, { w: 1, h: 1 }, { border: 'black' }), [0]);
});

const FLAT = gray(Array.from({ length: 4 }, () => [128, 128, 128, 128]));
test('dithering none: nearest level, and the 1 bit threshold decides', () => {
  assert.deepEqual(levels(FLAT, { w: 4, h: 4 }, { dither: 'none', threshold: 128 }), Array(16).fill(1));
  assert.deepEqual(levels(FLAT, { w: 4, h: 4 }, { dither: 'none', threshold: 129 }), Array(16).fill(0));
});
test('dithering ordered 4x4: 50% gray is a checkerboard (Bayer entries 8 and up turn white)', () => {
  assert.deepEqual(rows(levels(FLAT, { w: 4, h: 4 }, { dither: 'ordered' }), 4), ['0101', '1010', '0101', '1010']);
});
test('dithering Floyd-Steinberg: hand worked first row of 50% gray is 1 0 1 0, and the whole picture is about half white', () => {
  const l = levels(FLAT, { w: 4, h: 4 }, { dither: 'floyd' });
  assert.equal(rows(l, 4)[0], '1010');
  assert.ok(l.reduce((a, b) => a + b) >= 6 && l.reduce((a, b) => a + b) <= 10, String(l));
});

test('levels never leave the device set, whatever the settings', () => {
  const src = builtinPicture('scene', 61, 47, 4);
  for (const depth of [1, 2, 4]) for (const dither of ['none', 'floyd', 'ordered']) for (const fit of ['fit', 'fill', 'stretch']) {
    const max = Math.max(...levels(src, { w: 40, h: 30 }, { depth, dither, fit }));
    assert.ok(max < 2 ** depth, `${depth} ${dither} ${fit}`);
  }
  for (const dither of ['none', 'floyd', 'ordered']) assert.ok(Math.max(...levels(src, { w: 40, h: 30 }, { color: true, depth: 2, dither })) <= 3);
});

test('colour panel: the nearest of black 0, red 1, yellow 2, white 3', () => {
  const px = [[0, 0, 0], [250, 10, 10], [250, 250, 10], [255, 255, 255]];
  const src = { w: 4, h: 1, rgba: Uint8ClampedArray.from(px.flatMap((p) => [...p, 255])) };
  assert.deepEqual(levels(src, { w: 4, h: 1 }, { color: true, depth: 2, dither: 'none', fit: 'stretch' }), [0, 1, 2, 3]);
});

test('rotation: clockwise quarter turns; auto turns the picture when it then fills more of the panel', () => {
  const s = { w: 2, h: 1, rgba: Uint8ClampedArray.of(1, 1, 1, 255, 2, 2, 2, 255) };
  assert.deepEqual([rotateSource(s, 90).w, rotateSource(s, 90).h, rotateSource(s, 90).rgba[0], rotateSource(s, 90).rgba[4]], [1, 2, 1, 2]);
  assert.deepEqual([rotateSource(s, 180).rgba[0], rotateSource(s, 180).rgba[4]], [2, 1]);
  assert.deepEqual([rotateSource(s, 270).rgba[0], rotateSource(s, 270).rgba[4]], [2, 1]);
  assert.equal(pickRotation({ w: 4, h: 2 }, { w: 2, h: 4 }, 'auto'), 90);
  assert.equal(pickRotation({ w: 4, h: 2 }, { w: 4, h: 4 }, 'auto'), 0);
  assert.equal(pickRotation({ w: 4, h: 2 }, { w: 2, h: 4 }, 0), 0);
  assert.equal(compose(gray([[0, 0, 255, 255], [0, 0, 255, 255]]), { w: 2, h: 4 }, set({ rotate: 'auto' })).rotation, 90);
});

test('the PNG has the panel size and the depth chosen (gray), or the indexed colour type', async () => {
  const head = (png) => { const v = new DataView(png.buffer, png.byteOffset); return [v.getUint32(16), v.getUint32(20), png[24], png[25]]; };
  const src = builtinPicture('scene', 30, 20, 4);
  for (const depth of [1, 2, 4]) assert.deepEqual(head(await preparedPng(preparePicture(src, { w: 24, h: 16 }, set({ depth })))), [24, 16, depth, 0]);
  assert.deepEqual(head(await preparedPng(preparePicture(src, { w: 24, h: 16 }, set({ color: true, depth: 2 })))), [24, 16, 2, 3]);
});

test('the built-in pictures are the same bytes twice, differ from each other and are not flat', () => {
  const made = BUILTINS.map((k) => builtinPicture(k, 160, 120, 16));
  BUILTINS.forEach((k, i) => {
    assert.deepEqual(builtinPicture(k, 160, 120, 16).rgba, made[i].rgba, k);
    assert.ok(new Set(made[i].rgba).size > (k === 'lines' ? 1 : 8), `${k} is flat`);
    assert.equal(made[i].rgba.length, 160 * 120 * 4);
  });
  assert.notDeepEqual(made[0].rgba, made[1].rgba);
  assert.notDeepEqual(made[1].rgba, made[2].rgba);
  // the step wedge has one band for each level the device offers
  assert.notDeepEqual(builtinPicture('gradient', 160, 120, 4).rgba, builtinPicture('gradient', 160, 120, 16).rgba);
});

test('depth, refresh and Compare choices follow the hello', () => {
  assert.deepEqual(depthChoices(HELLO).map((c) => c.key), ['1', '2']);
  const pic = { ...HELLO, depths: [1], colors: ['black', 'white', 'red', 'yellow'], partial_depths: [], modes: ['full'] };
  assert.deepEqual(depthChoices(pic).map((c) => c.key), ['1', 'color']);
  const r1 = refreshChoices(HELLO, depthChoices(HELLO)[0]), r2 = refreshChoices(HELLO, depthChoices(HELLO)[1]);
  assert.deepEqual(r1.map((r) => r.enabled), [true, true, true]);
  assert.deepEqual(r2.map((r) => r.enabled), [true, false, true]);
  assert.match(r2[1].why, /partial updates at 1 bit only, not at 2 bit/);
  assert.match(refreshChoices(pic, depthChoices(pic)[0])[1].why, /no partial refresh/);
  assert.deepEqual(comparePlan(HELLO).map((s) => s.label), ['1 bit nearest level', '1 bit Floyd-Steinberg', '2 bit nearest level', '2 bit Floyd-Steinberg']);
  assert.deepEqual(comparePlan(pic).map((s) => s.label), ['1 bit nearest level', '1 bit Floyd-Steinberg', '4 colours nearest level', '4 colours Floyd-Steinberg']);
  assert.equal(partialLine({ ...HELLO, partial_depths: [1, 2, 4] }), 'Partial updates at: 1, 2 and 4 bit (16 grays)');
});

test('the report holds the device line, each send and the free line', () => {
  const t = reportText({ hello: HELLO, picture: 'p, 4 x 4 pixels', saw: 'smooth', sends: [{ label: 'Compare 1 of 4, 1 bit nearest level', settings: '1 bit', ok: true, answer: 'Full refresh: as requested (9 ms).', totalMs: 40 }] });
  assert.match(t, /^tiny-paper display, picture report\nDevice: TRMNL OG, 800 x 480, display firmware t\. Partial updates at: 1 bit\nPicture: p, 4 x 4 pixels\ntuning: none \(this device has no panel tuning\)\n1\. Compare 1 of 4, 1 bit nearest level: 1 bit\. Full refresh: as requested \(9 ms\)\. Total 40 ms\.\nWhat I saw: smooth$/);
});

test('the report holds the tuning from hello and the device log lines of each send, as the Sketch report does', () => {
  const hello = { ...HELLO, tuning: { gray_table: 'stock', gray_row: '' } };
  const logs = Array.from({ length: 10 }, (_, i) => `line ${i}`);
  const t = reportText({ hello, picture: 'p', saw: 'x', sends: [
    { label: '', settings: 'a', ok: true, answer: 'ok.', totalMs: 1, lines: ['panel full depth=4 ms=9', 'other'] },
    { label: '', settings: 'b', ok: true, answer: 'ok.', totalMs: 1, lines: logs }] });
  assert.match(t, /\nPicture: p\ntuning: gray_table=stock gray_row=\n1\. a\. ok\. Total 1 ms\.\n    \| panel full depth=4 ms=9\n    \| other\n2\. b\./);
  assert.match(t, /\| \(2 more device log lines cut; at most 8 are kept\)/);
  assert.match(reportText({ hello: HELLO, picture: 'p', saw: '', sends: [] }), /\ntuning: none \(this device has no panel tuning\)\n/);
});
