// SPDX-License-Identifier: MIT
// The mock devices' tuning keys are the spec's Tuning table: names, order, allowed values, defaults.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { MockDisplayDevice, X_KNOBS, applyTuning } from '../sketch.js';
import { fakeDisplay } from './fake-display.mjs';

const spec = readFileSync(new URL('../../docs/spec/usb-display.md', import.meta.url), 'utf8');
const section = spec.slice(spec.indexOf('\n## Tuning'), spec.indexOf('\n## ', spec.indexOf('\n## Tuning') + 5));
/** [{ name, allowed, def }] from the table rows. */
const rows = section.split('\n').map((l) => /^\| `(\w+)` \| ([^|]+) \| ([^|]+) \|/.exec(l)).filter(Boolean)
  .map((m) => ({ name: m[1], allowed: m[2].trim(), def: m[3].trim().replace(/`/g, '') }));

test('the spec table is found', () => { assert.ok(rows.length >= 17, `rows: ${rows.length}`); });

test('the mock X lists the spec keys in order, with their allowed values and defaults', () => {
  assert.deepEqual(X_KNOBS.map((k) => k.name), rows.map((r) => r.name));
  for (const [i, r] of rows.entries()) {
    const k = X_KNOBS[i];
    if (r.allowed.startsWith('text')) { assert.equal(k.text, true, `${r.name} is text`); assert.equal(r.def, 'none'); continue; }
    const range = /^(\d+) to (\d+)$/.exec(r.allowed);
    if (range) assert.deepEqual([k.min, k.max, k.words], [Number(range[1]), Number(range[2]), undefined], r.name);
    else assert.deepEqual(k.words, r.allowed.replace(/`/g, '').split(', '), r.name);
    assert.equal(String(k.words ? k.words[k.def - k.min] : k.def), r.def, `${r.name} default`);
  }
});

test('the hello of the mock X and of the fake display carry exactly the spec keys, at their defaults', () => {
  const want = rows.map((r) => r.name);
  const mock = new MockDisplayDevice('x').hello.tuning;
  const fake = fakeDisplay({ tuning: true }).hello.tuning;
  for (const t of [mock, fake]) {
    assert.deepEqual(Object.keys(t), want);
    for (const r of rows) assert.equal(String(t[r.name]), r.allowed.startsWith('text') ? '' : r.def, r.name);
  }
});

test('the mock X takes gray_row as the spec says: rows add up, wrong length refuses all, a table change clears', () => {
  const d = new MockDisplayDevice('x');
  const v = d.knobs, i = X_KNOBS.findIndex((k) => k.name === 'gray_row');
  assert.deepEqual(applyTuning(v, { gray_row: '5:DD.........' }), { gray_row: 'level 5: 11 passes given, the stock table has 9' });
  assert.equal(v[i], '');
  assert.deepEqual(applyTuning(v, { gray_row: '5:DD.......', gray_table: 'fastepd' }), { gray_row: 'level 5: 9 passes given, the fastepd table has 8' });
  assert.deepEqual(applyTuning(v, { gray_row: '5:DD......' }), {});
  assert.deepEqual(applyTuning(v, { gray_row: '2:LLLLLLLL' }), {});
  assert.equal(v[i], '2:LLLLLLLL 5:DD......');
  applyTuning(v, { gray_table: 'stock' });
  assert.equal(v[i], '', 'a table change puts the rows back');
});
