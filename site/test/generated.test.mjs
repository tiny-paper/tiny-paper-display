// SPDX-License-Identifier: MIT
// The pages written in Markdown are committed, and each is what its source makes: edit docs/, run `npm run pages` in
// site/. A stale page fails here. Also: the guide is not a copy of the byte-level reference.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { PAGES, render } from '../pages.mjs';

const SITE = join(dirname(fileURLToPath(import.meta.url)), '..');

for (const p of PAGES) {
  test(`${p.out} is what ${p.src} makes (run npm run pages in site/)`, () => {
    assert.equal(readFileSync(join(SITE, p.out), 'utf8'), render(p));
  });
}

test('the guide page shows no byte-level tables; the reference page has them', () => {
  const guide = readFileSync(join(SITE, 'api.html'), 'utf8');
  assert.doesNotMatch(guide, /A5 5A|CRC-16|show_begin|CBOR/);
  assert.match(readFileSync(join(SITE, 'protocol.html'), 'utf8'), /CRC-16/);
});

test('no marked-down link is left pointing into the repository tree', () => {
  for (const p of PAGES) assert.doesNotMatch(readFileSync(join(SITE, p.out), 'utf8'), /href="\.\.\//, p.out);
});

test('the generated pages load nothing: no script, no stylesheet, no image', () => {
  for (const p of PAGES) assert.doesNotMatch(readFileSync(join(SITE, p.out), 'utf8'), /<script|<link|<img|<iframe|\ssrc=/, p.out);
});
