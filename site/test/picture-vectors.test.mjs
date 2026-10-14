// SPDX-License-Identifier: MIT
// The shared vectors of the picture preparation (docs/spec/vectors/picture-prepare.json, made by gen_picture.mjs from this
// page's own code, read by python/tests/test_picture.py): the page still gives the pixels the Python package expects.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { preparePicture } from '../picture.js';

const DATA = JSON.parse(readFileSync(new URL('../../docs/spec/vectors/picture-prepare.json', import.meta.url), 'utf8'));
const SOURCES = Object.fromEntries(DATA.sources.map((s) => [s.name, { w: s.w, h: s.h, rgba: Uint8ClampedArray.from(Buffer.from(s.rgba, 'hex')) }]));

test('the vectors cover fit, border, rotate, depth, dither, threshold and colour', () => {
  const seen = (k) => new Set(DATA.cases.map((c) => c.settings[k]));
  assert.equal(seen('fit').size, 3);
  assert.equal(seen('border').size, 2);
  assert.ok(seen('rotate').size >= 5);
  assert.equal(seen('depth').size, 3);
  assert.equal(seen('dither').size, 3);
  assert.ok(seen('threshold').size >= 3);
  assert.equal(seen('color').size, 2);
});

for (const c of DATA.cases) {
  test(`vector ${c.id}: ${c.src} on ${c.panel.w}x${c.panel.h} ${JSON.stringify(c.settings)}`, () => {
    const pic = preparePicture(SOURCES[c.src], c.panel, c.settings);
    assert.equal(Buffer.from(pic.levels).toString('hex'), c.levels);
    assert.equal(pic.rotation, c.rotation);
  });
}
