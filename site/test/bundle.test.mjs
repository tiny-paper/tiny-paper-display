// SPDX-License-Identifier: MIT
// site/sketch.js and site/picture.js are generated from site/src: it is what `npm run build` makes of them, byte for byte.
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { BUNDLES, bundle } from '../build.mjs';

for (const out of Object.keys(BUNDLES)) {
  test(`${out} is what site/src makes: run \`npm run build\` in site/ after a change to src/`, async () => {
    const made = await bundle(out);
    const committed = readFileSync(new URL(`../${out}`, import.meta.url), 'utf8');
    assert.ok(made === committed, `site/${out} is not the bundle of site/src: run \`npm run build\` in site/ and commit it`);
  });
}
