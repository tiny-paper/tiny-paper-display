// SPDX-License-Identifier: MIT
// The Picture page (picture.html, picture-page.js, the bundle picture.js) against a fake serial port with a display
// firmware behind it: connect, the controls, the preview equal to what is sent, send in the refresh asked, refusals in words,
// Clear, Compare, a file that cannot be opened. Run: node --test site/test/*.test.mjs
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';

import { startPicturePage } from '../picture-page.js';
import { MockDisplayDevice, levelsToRgba, mockPort } from '../picture.js';
import { FakeImageData, fakeDocument } from './fake-dom.mjs';
import { fakeDisplay } from './fake-display.mjs';

const HTML = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'picture.html'), 'utf8');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(what, cond, ms = 4000) {
  for (const end = Date.now() + ms; !cond();) {
    if (Date.now() > end) assert.fail(`timed out waiting for ${what}`);
    await sleep(10);
  }
}

/** The page in a browser with Web Serial; `port`: what the picker hands out. The canvas keeps the last ImageData it was given. */
function open({ port, search = '', decodeImage = async () => { throw new Error('no decoder'); } } = {}) {
  const document = fakeDocument(HTML);
  const navigator = { serial: { requestPort: async () => port, addEventListener() {} } };
  const page = startPicturePage({ document, navigator, location: { search }, ImageData: FakeImageData, decodeImage, sleep: async () => {} });
  const el = (id) => document.getElementById(id);
  const shown = { img: null };
  el('canvas').getContext = () => ({ putImageData: (img) => { shown.img = img; } });
  for (const [id, v] of Object.entries({ fit: 'fit', border: 'white', rotate: 'auto', dither: 'floyd', threshold: '128', pause: '0' })) el(id).value = v; // the HTML's defaults
  const pick = (id, value) => { el(id).value = value; el(id).onchange?.(); };
  return { page, el, shown, pick };
}
async function connected(over = {}) {
  const dev = fakeDisplay({ w: 24, h: 16, ...over });
  const p = open({ port: dev.port });
  await p.page.connect();
  await until('the device line', () => p.el('pad').hidden === false);
  return { dev, ...p };
}
/** The levels of a PNG the page sent (our own writer: filter 0 on every row). */
function decode(png) {
  const v = new DataView(png.buffer, png.byteOffset), w = v.getUint32(16), h = v.getUint32(20), depth = png[24];
  let off = 8; const idat = [];
  while (off < png.length) { const n = v.getUint32(off), name = String.fromCharCode(...png.subarray(off + 4, off + 8)); if (name === 'IDAT') idat.push(png.subarray(off + 8, off + 8 + n)); off += n + 12; }
  const raw = inflateSync(Buffer.concat(idat)), stride = Math.ceil(w * depth / 8), out = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) out[y * w + x] = (raw[y * (stride + 1) + 1 + Math.floor(x * depth / 8)] >> (8 - depth * ((x * depth % 8) / depth + 1))) & ((1 << depth) - 1);
  return { w, h, depth, levels: out };
}

test('connect: the device line, the partial depths line, the controls for its depths, nothing sent yet', async () => {
  const { dev, el } = await connected();
  assert.equal(el('device').textContent, 'TRMNL OG, 24 x 16, display firmware 0.0.0+test.');
  assert.equal(el('partial').textContent, 'Partial updates at: 1 bit');
  assert.deepEqual(el('depth').children.map((o) => o.textContent), ['1 bit (black and white)', '2 bit (4 grays)']);
  assert.equal(el('depth').value, '2', 'the deepest depth to start with');
  assert.equal(el('send').disabled, true, 'no picture yet');
  assert.equal(dev.shows.length, 0);
  assert.match(HTML, /nowhere but to the device/);
});

test('a built-in picture: the preview is exactly the pixels the PNG carries, at the panel size and the depth chosen', async () => {
  const dev = new MockDisplayDevice();
  const { page, el, shown, pick } = open({ port: mockPort(dev) });
  await page.connect();
  await until('pad', () => !el('pad').hidden);
  el('b-scene').onclick();
  pick('depth', '1'); pick('dither', 'floyd');
  assert.equal(el('canvas').width, 16); assert.equal(el('canvas').height, 8);
  await el('send').onclick();
  await until('the answer', () => el('status').dataset.state === 'ok' && dev.shows.length === 1);
  const sent = decode(dev.shows[0].png);
  assert.deepEqual([sent.w, sent.h, sent.depth], [16, 8, 1]);
  assert.deepEqual(Array.from(shown.img.data), Array.from(levelsToRgba(sent.levels, 1, false)), 'the preview shows what was sent');
  assert.ok(sent.levels.some((v) => v) && sent.levels.some((v) => !v));
  pick('depth', '2'); pick('dither', 'ordered');
  await el('send').onclick();
  await until('second', () => dev.shows.length === 2);
  const two = decode(dev.shows[1].png);
  assert.equal(two.depth, 2);
  assert.ok(Math.max(...two.levels) <= 3 && Math.max(...two.levels) > 1);
  assert.deepEqual(Array.from(shown.img.data), Array.from(levelsToRgba(two.levels, 2, false)));
});

test('refresh: auto sends no mode, partial and full as chosen; partial is disabled with the reason at a depth without it', async () => {
  const { dev, el, pick } = await connected();
  el('b-lines').onclick();
  pick('depth', '1');
  assert.deepEqual(el('refresh').children.map((o) => o.disabled), [false, false, false]);
  for (const [want, mode] of [['auto', undefined], ['partial', 'partial'], ['full', 'full']]) {
    pick('refresh', want);
    const n = dev.shows.length;
    await el('send').onclick();
    await until(want, () => dev.shows.length === n + 1 && el('send').disabled === false);
    assert.deepEqual([dev.shows.at(-1).mode, dev.shows.at(-1).w, dev.shows.at(-1).h, dev.shows.at(-1).bits], [mode, 24, 16, 1]);
  }
  assert.match(el('status').textContent, /^Asked for 1 bit, Floyd-Steinberg, whole picture, border white, rotated 0 \(auto\), full refresh\. Full refresh: as requested \(9 ms\)\. Area 24 x 16 at 0, 0\. Panel 9 ms, \d+ ms in all\.$/);
  pick('refresh', 'partial');
  pick('depth', '2');
  assert.equal(el('refresh').value, 'auto', 'partial is not offered at 2 bit: back to auto');
  assert.equal(el('refresh').children[1].disabled, true);
  assert.match(el('refreshwhy').textContent, /^Partial is not available: this device does partial updates at 1 bit only, not at 2 bit\.$/);
});

test('a refused send is said in words, the device keeps no claim of success', async () => {
  const { dev, el } = await connected();
  el('b-gradient').onclick();
  dev.refuse = { err: 34, msg: 'depth 2 not taken' };
  await el('send').onclick();
  await until('the refusal', () => el('status').dataset.state === 'error');
  assert.match(el('status').textContent, /Not shown: The panel cannot show these colors or bit depth: depth 2 not taken\./);
  assert.equal(el('status').className, 'warn');
  assert.match(el('report').value, /Not shown: The panel cannot show these colors/);
  assert.equal(el('send').disabled, false, 'the page is usable again');
});

test('Compare: four full refreshes, 1 bit nearest, 1 bit Floyd-Steinberg, deepest nearest, deepest Floyd-Steinberg, each said while on the panel', async () => {
  const { dev, el } = await connected();
  el('b-scene').onclick();
  const lines = [];
  let text = '';
  Object.defineProperty(el('comparestatus'), 'textContent', { get: () => text, set: (v) => { text = v; lines.push(v); } });
  await el('compare').onclick();
  await until('four shows', () => dev.shows.length === 4 && el('compare').disabled === false);
  assert.deepEqual(dev.shows.map((s) => [s.mode, s.bits, s.w, s.h]), [['full', 1, 24, 16], ['full', 1, 24, 16], ['full', 2, 24, 16], ['full', 2, 24, 16]]);
  assert.match(el('comparestatus').textContent, /^Compare 4 of 4: 2 bit Floyd-Steinberg is on the panel now\. That was the last one\.$/);
  assert.ok(lines.some((l) => /^Compare 1 of 4: 1 bit nearest level is on the panel now\. Next one in 0 s\.$/.test(l)));
  const r = el('report').value;
  for (const t of ['1. Compare 1 of 4, 1 bit nearest level: 1 bit, nearest level (threshold 128)', '2. Compare 2 of 4, 1 bit Floyd-Steinberg', '3. Compare 3 of 4, 2 bit nearest level: 2 bit gray', '4. Compare 4 of 4, 2 bit Floyd-Steinberg', 'full refresh. Full refresh: as requested (9 ms)', 'What I saw: ']) assert.ok(r.includes(t), t);
  assert.match(r, /^tiny-paper display, picture report\nDevice: TRMNL OG, 24 x 16, display firmware 0\.0\.0\+test\. Partial updates at: 1 bit\nPicture: Photo-like scene, 24 x 16 pixels\n/);
  el('saw').value = 'step 3 best'; el('saw').oninput();
  assert.match(el('report').value, /What I saw: step 3 best$/);
  assert.equal(el('compare').disabled, false);
});

test('Compare stops at the first refusal and says where', async () => {
  const { dev, el } = await connected();
  el('b-scene').onclick();
  dev.refuse = { err: 14, msg: '' };
  await el('compare').onclick();
  await until('stop', () => /^Compare stopped at 1 of 4/.test(el('comparestatus').textContent) && el('compare').disabled === false);
  assert.equal(dev.shows.length, 0);
});

test('Clear asks the device for a full refresh to white and says what it did', async () => {
  const dev = new MockDisplayDevice();
  const { page, el } = open({ port: mockPort(dev) });
  await page.connect();
  await until('pad', () => !el('pad').hidden);
  await el('clear').onclick();
  await until('clear', () => dev.calls.some((c) => c.cmd === 11) && el('clear').disabled === false);
  assert.match(el('status').textContent, /^Asked for a full refresh to white\. Full refresh: as requested \(0 ms\)\. Panel 0 ms, \d+ ms in all\.$/);
});

test('a file the browser cannot open is said in words; a file that opens is prepared; the preview without a device works', async () => {
  const dev = fakeDisplay({ w: 8, h: 8 });
  const p = open({ port: dev.port, decodeImage: async (f) => { if (f.bad) throw new Error('decode'); return { w: 2, h: 2, rgba: Uint8ClampedArray.from([0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255]) }; } });
  await p.page.load({ bad: true }, 'x.png');
  assert.match(p.el('source').textContent, /^Connect a device first/);
  await p.page.connect();
  await until('pad', () => !p.el('pad').hidden);
  await p.page.load({ bad: true }, 'x.png');
  assert.equal(p.el('source').textContent, 'This file could not be opened as a picture. Try a PNG or JPEG.');
  await p.page.load({}, 'two.png');
  assert.equal(p.el('source').textContent, 'Picture: two.png, 2 x 2 pixels.');
  assert.equal(p.el('send').disabled, false);
  assert.equal(p.shown.img.width, 8);
  p.el('zoom').onclick();
  assert.equal(p.el('canvas').style.width, '8px');
  const mock = open({ search: '?mock=picpak' });
  await mock.page.ready;
  assert.match(mock.el('device').textContent, /^PicPak, 400 x 300/);
  assert.deepEqual(mock.el('depth').children.map((o) => o.value), ['1', 'color']);
  assert.equal(mock.el('refresh').children[1].disabled, true, 'the PicPak has no partial refresh');
  assert.equal(mock.el('preview').hidden, false);
});
