// SPDX-License-Identifier: MIT
// site/sketch.js is generated from site/src: it is what `npm run build` makes of them, byte for byte.
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { bundle } from '../build.mjs';

test('sketch.js is what site/src makes: run `npm run build` in site/ after a change to src/', async () => {
  const made = await bundle();
  const committed = readFileSync(new URL('../sketch.js', import.meta.url), 'utf8');
  assert.ok(made === committed, 'site/sketch.js is not the bundle of site/src: run `npm run build` in site/ and commit it');
});
