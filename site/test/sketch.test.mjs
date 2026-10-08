// SPDX-License-Identifier: MIT
// The Sketch page (sketch.html, sketch-page.js, the bundle sketch.js) against a fake serial port with a display
// firmware behind it: connect, a stroke as one partial show of an aligned rectangle, the full refresh after N, a
// refusal in words, a device that is not the display firmware, a pulled cable, the preview without a device, a
// browser without Web Serial. Run: node --test site/test/*.test.mjs
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

import { startSketchPage } from '../sketch-page.js';
import { fingerprintOf } from '../sketch.js';
import { inflateSync } from 'node:zlib';
import { FakeImageData, fakeDocument, pointer } from './fake-dom.mjs';
import { fakeDisplay } from './fake-display.mjs';

const HTML = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'sketch.html'), 'utf8');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(what, cond, ms = 4000) {
  for (const end = Date.now() + ms; !cond();) {
    if (Date.now() > end) assert.fail(`timed out waiting for ${what}`);
    await sleep(10);
  }
}

/** The page in a browser with (or without) Web Serial; `dev`: what the port picker hands out. */
function open({ dev = null, search = '', serial = true, storage = null, clipboard = undefined } = {}) {
  const document = fakeDocument(HTML);
  const listeners = {};
  const navigator = serial ? { serial: {
    requestPort: async () => { if (!dev) throw new Error('nothing picked'); return dev.port; },
    addEventListener: (type, f) => { listeners[type] = f; },
  } } : {};
  if (clipboard) navigator.clipboard = clipboard;
  const resizers = [];
  const page = startSketchPage({ document, navigator, location: { search }, ImageData: FakeImageData, localStorage: storage, onResize: (f) => resizers.push(f) });
  const el = (id) => document.getElementById(id);
  const stroke = (points) => {
    el('canvas').dispatch('pointerdown', pointer(...points[0]));
    for (const p of points.slice(1)) el('canvas').dispatch('pointermove', pointer(...p));
    el('canvas').dispatch('pointerup', pointer(...points.at(-1)));
  };
  return { page, el, stroke, listeners, resize: () => resizers.forEach((f) => f()) };
}
/** The pixel levels of a grayscale panel PNG made by the page (filter 0, no interlace). */
function levels(png) {
  const v = new DataView(png.buffer, png.byteOffset, png.byteLength);
  const w = v.getUint32(16), h = v.getUint32(20), depth = png[24];
  const idat = [];
  for (let off = 8; off < png.length;) {
    const len = v.getUint32(off), name = String.fromCharCode(...png.subarray(off + 4, off + 8));
    if (name === 'IDAT') idat.push(png.subarray(off + 8, off + 8 + len));
    off += len + 12;
  }
  const raw = inflateSync(Buffer.concat(idat)), stride = Math.ceil(w * depth / 8);
  const out = [];
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) out.push((raw[y * (stride + 1) + 1 + Math.floor(x * depth / 8)] >> (8 - depth - (x * depth) % 8)) & ((1 << depth) - 1));
  return out;
}

/** A browser that remembers the Grays switch as on for this board. */
const graysOn = (board) => { const m = new Map([[`tp-sketch-grays-${board}`, '1']]); return { getItem: (k) => m.get(k) ?? null, setItem: (k, v) => m.set(k, v) }; };
// each test its own panel height: the page keeps one drawing per panel shape while it is loaded
async function connected(h, over = {}) {
  const dev = fakeDisplay({ h, ...over });
  // a device that does gray partial updates is tried with the Grays switch on (1 bit is the default: see the test of that)
  const p = open({ dev, storage: over.partial_depths && over.partial_depths.includes(4) ? graysOn(over.board ?? 'trmnl_og') : null });
  await p.page.connect();
  await until('the whole drawing on connect', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
  return { dev, ...p };
}

test('connect: the device named, a canvas of the panel\'s pixels, its two colours, the whole drawing sent once in full', async () => {
  const { dev, el } = await connected(480);
  assert.match(el('device').textContent, /^TRMNL OG, 800 x 480, display firmware 0\.0\.0\+test\.$/);
  assert.equal(el('partial').textContent, 'Partial updates at: 1 bit');
  assert.equal(el('partial').hidden, false);
  assert.equal(el('pad').hidden, false);
  assert.deepEqual([el('canvas').width, el('canvas').height], [800, 480]);
  assert.deepEqual(el('swatches').children.map((b) => b.title), ['black', 'white']);
  assert.deepEqual(el('brushes').children.map((b) => b.getAttribute('aria-label')), ['Small', 'Medium', 'Large']);
  assert.deepEqual(el('brushes').children.map((b) => b.title), ['Small', 'Medium', 'Large']);
  assert.deepEqual(el('brushes').children.map((b) => b.children[1].textContent), ['Small', 'Medium', 'Large'], 'the word stays for assistive technology');
  assert.equal(el('connect').textContent, 'Disconnect');
  assert.equal(el('install').hidden, true);
  assert.equal(el('nosupport').hidden, true);
  assert.deepEqual(dev.shows[0], { mode: 'full', x: undefined, y: undefined, w: 800, h: 480, bits: 1, signature: '89504e470d0a1a0a' });
  assert.match(el('status').textContent, /^Asked for a full refresh of the whole drawing \(800 x 480\)\. Full refresh: as requested \(9 ms\)\. \d+ ms in all\.$/);
  assert.equal(el('fullafter').value, '30');
});

test('a stroke arrives as one partial show: x and width in steps of 8, around the stroke, far smaller than the panel', async () => {
  const { dev, el, stroke } = await connected(472);
  stroke([[101, 50], [117, 55], [133, 61]]);
  await until('the stroke', () => dev.shows.length === 2 && el('status').dataset.state === 'ok');
  await sleep(400);
  assert.equal(dev.shows.length, 2, 'one show per stroke');
  const s = dev.shows[1];
  assert.equal(s.mode, 'partial');
  assert.equal(s.x % 8, 0);
  assert.equal(s.w % 8, 0);
  assert.ok(s.x <= 101 && s.x + s.w >= 133 && s.y <= 50 && s.y + s.h >= 61, JSON.stringify(s));
  assert.ok(s.x >= 88 && s.w <= 64 && s.h <= 32, JSON.stringify(s));
  assert.equal(s.bits, 1);
  assert.match(el('status').textContent, new RegExp(`^Asked for a partial update of ${s.w} x ${s.h} pixels at ${s.x}, ${s.y}\\. Changed area refreshed: changed pixels \\(7 ms\\)\\. \\d+ ms in all\\.$`));
  assert.ok(el('canvas').puts.length > 1, 'the canvas shows the stroke');
  assert.equal(el('undo').disabled, false);
  // undo is a change too, and it makes pixels lighter: this device has `inverse` at 1 bit, so the same area goes out as that
  el('undo').onclick();
  await until('the undo', () => dev.shows.length === 3);
  assert.deepEqual([dev.shows[2].mode, dev.shows[2].x, dev.shows[2].w], ['inverse', s.x, s.w]);
  assert.equal(el('undo').disabled, true);
});

test('full after N: the third send after two partial updates is a full refresh by itself; the button asks for one', async () => {
  const { dev, el, stroke } = await connected(464);
  el('fullafter').value = '2';
  el('fullafter').onchange();
  stroke([[40, 40], [60, 40]]);
  await until('stroke 1', () => dev.shows.length === 2);
  stroke([[200, 100], [220, 130]]);
  await until('stroke 2 and the full refresh', () => dev.shows.length === 4);
  assert.deepEqual(dev.shows.slice(1).map((s) => s.mode), ['partial', 'partial', 'full']);
  assert.deepEqual([dev.shows[3].w, dev.shows[3].h], [800, 464]);
  await until('the answer', () => /^Asked for a full refresh/.test(el('status').textContent) && el('status').dataset.state === 'ok');
  el('full').onclick();
  await until('the asked full refresh', () => dev.shows.length === 5);
  assert.equal(dev.shows[4].mode, 'full');
  // a value that is no count: the default again, never "always full"
  el('fullafter').value = 'abc';
  el('fullafter').onchange();
  stroke([[300, 300], [310, 300]]);
  await until('a stroke after the bad value', () => dev.shows.length === 6);
  assert.equal(dev.shows[5].mode, 'partial');
});

test('a refusal is said in plain words and the drawing is not lost', async () => {
  const { dev, el, stroke } = await connected(456);
  dev.refuse = { err: 37, msg: 'rect' };
  stroke([[40, 40], [60, 40]]);
  await until('the refusal', () => el('status').dataset.state === 'error');
  assert.match(el('status').textContent, /Not shown: The rectangle is outside the panel or misaligned: rect\. The device may show something else now: the next update is the whole drawing/);
  assert.equal(el('status').className, 'warn');
  el('full').onclick();
  await until('the whole drawing again', () => dev.shows.at(-1).mode === 'full' && dev.shows.length === 2 && el('status').dataset.state === 'ok');
});

test('a device that is not the display firmware: nothing to draw on, and how to install', async () => {
  for (const dev of [fakeDisplay({ project: 'tiny-paper' }), fakeDisplay(null)]) {
    const { page, el } = open({ dev });
    await page.connect();
    assert.equal(el('install').hidden, false);
    assert.equal(el('installwhy').textContent, 'This device does not answer as the display firmware.');
    assert.match(el('device').textContent, dev.hello ? /does not run the display firmware/ : /did not answer/);
    assert.equal(el('device').className, 'warn');
    assert.equal(el('pad').hidden, true);
    assert.equal(el('connect').disabled, false);
    assert.equal(dev.port.readable, null, 'the port is closed again');
  }
  assert.match(HTML, /<p id="install"[^>]*hidden>[^]*?<a href="index\.html">install it on the Flash page<\/a>/);
});

test('no port picked, a port that does not open: plain words, the button works again', async () => {
  const none = open({});
  await none.page.connect();
  assert.match(none.el('device').textContent, /^No port was chosen\./);
  assert.equal(none.el('connect').disabled, false);
  const dev = fakeDisplay();
  dev.port.open = async () => { throw new Error('busy'); };
  const busy = open({ dev });
  await busy.page.connect();
  assert.match(busy.el('device').textContent, /^The port could not be opened\./);
  assert.equal(busy.el('install').hidden, true);
});

test('the cable is pulled: said on the page, the drawing stays, connecting again sends it whole', async () => {
  const { dev, el, stroke, listeners, page } = await connected(448);
  stroke([[40, 40], [60, 40]]);
  await until('the stroke', () => dev.shows.length === 2);
  listeners.disconnect({ target: dev.port });
  await until('the note', () => /unplugged/.test(el('device').textContent));
  assert.equal(el('full').disabled, true);
  assert.equal(el('connect').textContent, 'Connect');
  assert.equal(el('pad').hidden, false);
  stroke([[80, 80], [90, 80]]);
  await sleep(300);
  assert.equal(dev.shows.length, 2, 'nothing is sent without a device');
  await page.connect();
  await until('the whole drawing again', () => dev.shows.length === 3);
  assert.deepEqual([dev.shows[2].mode, dev.shows[2].w, dev.shows[2].h], ['full', 800, 448]);
});

test('preview (?mock=): a pretend device answers with no Web Serial at all, per panel', async () => {
  const og = open({ search: '?mock=og', serial: false });
  await og.page.ready;
  assert.equal(og.el('preview').hidden, false);
  assert.equal(og.el('preview').textContent, 'Preview: no device is attached. A pretend TRMNL OG answers, so nothing reaches a panel.');
  assert.deepEqual([og.el('canvas').width, og.el('canvas').height], [800, 480]);
  assert.equal(og.el('pad').hidden, false);
  await until('the first full refresh', () => /^Asked for a full refresh/.test(og.el('status').textContent) && og.el('status').dataset.state === 'ok');
  og.stroke([[300, 200], [340, 240]]);
  await until('the stroke', () => /^Asked for a partial update of \d+ x \d+ pixels at \d+, \d+\. Changed area refreshed/.test(og.el('status').textContent));

  const x = open({ search: '?mock=x', serial: false, storage: graysOn('trmnl_x') });
  await x.page.ready;
  assert.deepEqual([x.el('canvas').width, x.el('canvas').height], [1872, 1404]);
  assert.equal(x.el('swatches').children.length, 16, 'the X answers gray partial depths: all 16 grays');
  assert.equal(x.el('note').hidden, true, 'no black-and-white note where the device has gray partials');
  assert.match(og.el('note').textContent, /cannot show grays, so the sketch is black and white/);
  await until('the first full refresh', () => /^Asked for a full refresh/.test(x.el('status').textContent) && x.el('status').dataset.state === 'ok');
  x.page.state.pen.color = 5;
  x.stroke([[400, 300], [440, 340]]);
  await until('the gray stroke', () => /^Asked for a partial update of \d+ x \d+ pixels at \d+, \d+\. Changed area refreshed/.test(x.el('status').textContent));
  assert.equal(x.page.state.sender.ink.depth, 4);

  const pic = open({ search: '?mock=picpak', serial: false });
  await pic.page.ready;
  assert.deepEqual([pic.el('canvas').width, pic.el('canvas').height], [400, 300]);
  assert.deepEqual(pic.el('swatches').children.map((b) => b.title), ['black', 'white', 'red', 'yellow']);
  assert.match(pic.el('note').textContent, /no partial refresh/);
  assert.equal(pic.el('partial').textContent, 'Partial updates at: none (full refreshes only)');
  assert.equal(pic.el('fullafterrow').hidden, true);
  pic.stroke([[30, 30], [60, 60]]);
  await sleep(300);
  assert.equal(pic.el('status').dataset.state, 'idle', 'no partial panel: a stroke sends nothing');
  pic.el('full').onclick();
  await until('the full refresh', () => /Full refresh: as requested/.test(pic.el('status').textContent));

  const bad = open({ search: '?mock=nope', serial: false });
  await bad.page.ready;
  assert.match(bad.el('preview').textContent, /There is no preview panel "nope"/);
  assert.equal(bad.el('pad').hidden, true);
});

test('a browser without Web Serial: the note with the way to the preview, no Connect; with it: no note', () => {
  const without = open({ serial: false });
  assert.equal(without.el('nosupport').hidden, false);
  assert.equal(without.el('connect').disabled, true);
  assert.match(HTML, /<p id="nosupport"[^>]*hidden>This browser cannot reach a USB device \(no Web Serial\)\. Use Chrome or Edge on a\s+computer\. <a href="sketch\.html\?mock=x">/);
  const withSerial = open({});
  assert.equal(withSerial.el('nosupport').hidden, true);
  assert.equal(withSerial.el('connect').disabled, false);
  assert.equal(withSerial.el('pad').hidden, true);
});

test('a device with gray partial depths: 16 swatches, strokes go as partial updates at 4 bit; without them black and white and the note', async () => {
  const gray = await connected(1404, { board: 'trmnl_x', w: 1872, depths: [1, 2, 4], partial_depths: [1, 2, 4], modes: ['full', 'partial', 'partial_all', 'inverse', 'passes'], max_png: 1319644 });
  assert.equal(gray.el('partial').textContent, 'Partial updates at: 1, 2 and 4 bit (16 grays)');
  assert.equal(gray.el('swatches').children.length, 16);
  assert.equal(gray.el('note').hidden, true);
  gray.page.state.pen.color = 6;
  gray.stroke([[300, 200], [340, 240]]);
  await until('the partial show', () => gray.dev.shows.length === 2);
  assert.equal(gray.dev.shows[1].mode, 'partial');
  assert.equal(gray.dev.shows[1].bits, 4);
  const bw = await connected(1403, { board: 'trmnl_x', w: 1872, depths: [1, 2, 4], partial_depths: [1] });
  assert.equal(bw.el('partial').textContent, 'Partial updates at: 1 bit', 'what the device reported, so a device without grays is seen on the page');
  assert.equal(bw.el('swatches').children.length, 2);
  assert.equal(bw.el('note').hidden, false);
});

const shown = (dev, n) => until(`${n} shows`, () => dev.shows.length >= n);
const said = (el, re) => until(`the status ${re}`, () => re.test(el('status').textContent) && el('status').dataset.state === 'ok');

test('Clear: the whole panel goes out in one full show, at 1 bit and at 4 bit, never as a partial update', async () => {
  for (const [h, over, bits] of [[440, {}, 1], [441, { board: 'trmnl_x', w: 1872, depths: [1, 2, 4], partial_depths: [1, 2, 4], modes: ['full', 'partial', 'partial_all', 'inverse', 'passes'] }, 4]]) {
    const { dev, el, stroke } = await connected(h, over);
    const w = over.w ?? 800;
    stroke([[101, 50], [117, 55], [133, 61]]);
    await shown(dev, 2);
    el('clear').onclick();
    await shown(dev, 3);
    const s = dev.shows[2];
    assert.deepEqual([s.mode, s.x, s.y, s.w, s.h, s.bits], ['full', undefined, undefined, w, h, bits]);
    await said(el, new RegExp(`^Asked for a full refresh of the whole drawing \\(${w} x ${h}\\)\\. Full refresh: as requested \\(9 ms\\)\\. \\d+ ms in all\\. Clear refreshes the whole panel: a partial update would leave faint traces of what was removed\\.$`));
    await sleep(400);
    assert.equal(dev.shows.length, 3, 'nothing more is sent after the Clear');
  }
});

test('Erase and Undo at 1 bit: the strongest region clean the device offers, said in the status line', async () => {
  const cases = [
    [442, ['full', 'partial', 'inverse', 'passes'], 'inverse', undefined, /^Asked for an inverse update of \d+ x \d+ pixels at \d+, \d+, to clean the pixels that got lighter\. Inverse then picture refreshed: as requested/],
    [443, ['full', 'partial', 'passes', 'partial_all'], 'passes', 2, /^Asked for a repeated update \(2 passes\) of \d+ x \d+ pixels at \d+, \d+, to clean the pixels that got lighter\. Repeated area refresh: as requested/],
    [444, ['full', 'partial', 'partial_all'], 'partial_all', undefined, /^Asked for an update of every pixel of \d+ x \d+ pixels at \d+, \d+, to clean the pixels that got lighter\. Whole area refreshed: as requested/],
  ];
  for (const [h, modes, mode, passes, re] of cases) {
    const { dev, el, stroke } = await connected(h, { modes });
    stroke([[101, 50], [117, 55], [133, 61]]);
    await shown(dev, 2);
    assert.equal(dev.shows[1].mode, 'partial', 'drawing black on white darkens: a plain partial update');
    el('eraser').onclick();
    stroke([[101, 50], [117, 55], [133, 61]]);
    await shown(dev, 3);
    assert.deepEqual([dev.shows[2].mode, dev.shows[2].passes], [mode, passes]);
    await said(el, re);
  }
});

test('Erase and Undo where nothing stronger than partial exists (4 bit): counted, and the third brings the full refresh forward, said in the status line', async () => {
  const gray = { board: 'trmnl_x', w: 1872, depths: [1, 2, 4], partial_depths: [1, 2, 4], modes: ['full', 'partial', 'partial_all', 'inverse', 'passes'] };
  const { dev, el, stroke } = await connected(445, gray);
  for (const y of [100, 200, 300]) { stroke([[101, y], [133, y + 6]]); await shown(dev, dev.shows.length + 1); }
  assert.equal(dev.shows.length, 4);
  el('undo').onclick();
  await shown(dev, 5);
  assert.equal(dev.shows[4].mode, 'partial', 'no inverse at 4 bit, whatever the modes list says');
  await said(el, /^Asked for a partial update of .* Changed area refreshed: changed pixels \(7 ms\)\. \d+ ms in all\. Erasing by a partial update can leave faint traces at 4 bit: a full refresh follows after 3 such sends \(1 so far\)\.$/);
  el('undo').onclick();
  await shown(dev, 6);
  await said(el, /\(2 so far\)\.$/);
  el('undo').onclick();
  await shown(dev, 7);
  assert.equal(dev.shows[6].mode, 'partial');
  await shown(dev, 8);
  const f = dev.shows[7];
  assert.deepEqual([f.mode, f.x, f.w, f.h, f.bits], ['full', undefined, 1872, 445, 4]);
  await said(el, /^Asked for a full refresh of the whole drawing \(1872 x 445\)\. Full refresh: as requested \(9 ms\)\. \d+ ms in all\. Full refresh by itself: 3 sends in a row erased pixels by a partial update, which leaves faint traces\.$/);
  await sleep(400);
  assert.equal(dev.shows.length, 8, 'the count starts again after the full refresh');
});

// ---- the Debug section: a pasted configuration, per-key results, test patterns and the report
const XDEV = { board: 'trmnl_x', w: 1872, depths: [1, 2, 4], partial_depths: [1, 2, 4], modes: ['full', 'partial', 'partial_all', 'inverse', 'passes'], max_png: 1319644, tuning: true };
const apply = async (p, config) => { p.el('debugjson').value = typeof config === 'string' ? config : JSON.stringify(config); p.el('debugapply').onclick(); };
const resultsHave = (p, re) => until(`the results ${re}`, () => re.test(p.el('debugresults').textContent));

test('Debug: valid JSON is applied, the device part as one set, the page part in effect: strokes go out at the chosen depth and mode', async () => {
  const { dev, el, stroke, page } = await connected(450, XDEV);
  assert.equal(el('debugnone').hidden, true);
  await apply({ el }, { tuning: { gray_table: 'fastepd', gray_dark: 2 }, sketch: { depth: 2, mode: 'partial', full_after: 5, erase: 'partial' } });
  await resultsHave({ el }, /sketch\.erase: applied: partial/);
  assert.equal(dev.sets.length, 1, 'the device part is one set');
  assert.deepEqual(dev.sets[0], { tuning: { gray_table: 'fastepd', gray_dark: 2 } });
  assert.deepEqual(dev.knobs.slice(0, 3), [0, 1, 2]);
  const text = el('debugresults').textContent;
  assert.match(text, /tuning\.gray_table: applied: fastepd/);
  assert.match(text, /tuning\.gray_dark: applied: 2/);
  assert.match(text, /sketch\.depth: applied: 2 bit/);
  assert.equal(el('swatches').children.length, 4, 'a drawing at 2 bit has 4 grays');
  assert.equal(el('fullafter').value, '5');
  await until('the whole drawing at 2 bit', () => dev.shows.length === 2 && dev.shows[1].bits === 2);
  stroke([[300, 200], [340, 240]]);
  await shown(dev, 3);
  assert.deepEqual([dev.shows[2].mode, dev.shows[2].bits], ['partial', 2]);
  // depth 1 with a named mode: strokes go out in that mode
  await apply({ el }, { sketch: { depth: 1, mode: 'passes' } });
  await until('the whole drawing at 1 bit', () => dev.shows.length === 4);
  stroke([[600, 300], [640, 340]]); // the drawing came along: the same place would change nothing
  await shown(dev, 5);
  assert.deepEqual([dev.shows[4].mode, dev.shows[4].passes, dev.shows[4].bits], ['passes', 2, 1]);
  assert.equal(dev.sets.length, 1, 'a page-only configuration sends no set');
});

test('Debug: a refused key is shown with the device\'s words and the rest is still applied', async () => {
  const { dev, el } = await connected(451, XDEV);
  await apply({ el }, { tuning: { gray_dark: 99, gray_light: 5, nonsense: 1, gray_table: 'linear' }, sketch: { full_after: 7, mode: 'nope', erase: 'full' } });
  await resultsHave({ el }, /sketch\.erase: applied: full/);
  const t = el('debugresults').textContent;
  assert.match(t, /tuning\.gray_dark: refused: out of range; allowed 0 to 14/);
  assert.match(t, /tuning\.gray_table: refused: not one of the words; allowed fastepd, stock/);
  assert.match(t, /tuning\.nonsense: refused: no such key; known: gray_table gray_partial/);
  assert.match(t, /tuning\.gray_light: applied: 5/);
  assert.match(t, /sketch\.mode: refused: the device lists full, partial, partial_all, inverse, passes; or auto/);
  assert.match(t, /sketch\.full_after: applied: 7/);
  assert.equal(dev.knobs[3], 5);
  assert.equal(dev.knobs[2], 0, 'a refused value changes nothing');
  assert.equal(el('fullafter').value, '7');
  assert.match(el('report').value, /gray_light=5/, 'the report shows the tuning the device reports now');
});

test('Debug: text that is not valid JSON is reported with its place and nothing is sent', async () => {
  const { dev, el } = await connected(452, XDEV);
  const shows = dev.shows.length;
  await apply({ el }, '{\n  "tuning": {\n    "gray_dark": 2,\n  }\n}');
  await resultsHave({ el }, /Not valid JSON/);
  assert.match(el('debugresults').textContent, /\(line 4, column 3\)\. Nothing was sent\./);
  await apply({ el }, '[1, 2]');
  await resultsHave({ el }, /must be a JSON object/);
  await apply({ el }, '{ "tuning": 5 }');
  await resultsHave({ el }, /"tuning" must be an object of keys, or "defaults"/);
  await apply({ el }, '{ "tunning": {} }');
  await resultsHave({ el }, /tunning: refused: no such part/);
  await sleep(200);
  assert.equal(dev.sets.length, 0);
  assert.equal(dev.shows.length, shows);
});

test('Debug: a device without panel tuning says so, refuses the tuning keys without sending, and the sketch part works', async () => {
  const { dev, el } = await connected(453, { board: 'trmnl_og' });
  assert.equal(el('debugnone').hidden, false);
  await apply({ el }, { tuning: { gray_dark: 1 }, sketch: { full_after: 9 } });
  await resultsHave({ el }, /sketch\.full_after: applied: 9/);
  assert.match(el('debugresults').textContent, /tuning\.gray_dark: refused: this device has no panel tuning/);
  assert.equal(dev.sets.length, 0);
  assert.match(el('report').value, /tuning: none \(this device has no panel tuning\)/);
});

test('Debug: Reset puts both sides back to their defaults', async () => {
  const { dev, el } = await connected(454, XDEV);
  await apply({ el }, { tuning: { gray_dark: 7, clear: 'slow' }, sketch: { depth: 1, full_after: 3 } });
  await until('applied', () => dev.knobs[2] === 7 && el('swatches').children.length === 2);
  el('debugreset').onclick();
  await resultsHave({ el }, /sketch: defaults restored/);
  assert.deepEqual(dev.sets.at(-1), { tuning: 'defaults' });
  assert.deepEqual(dev.knobs.slice(2, 3).concat(dev.knobs[12]), [0, 0]);
  assert.equal(el('swatches').children.length, 16);
  assert.equal(el('fullafter').value, '30');
  assert.match(el('report').value, /sketch: depth=auto \(4 bit in use\) mode=auto full_after=30 erase=auto/);
});

test('Debug: the report holds the device, the tuning, the page settings, what was seen and the last 12 status lines with their times', async () => {
  let copied = '';
  const dev = fakeDisplay({ h: 455, ...XDEV });
  const { el, stroke, page } = open({ dev, storage: graysOn('trmnl_x'), clipboard: { writeText: async (t) => { copied = t; } } });
  await page.connect();
  await until('connected', () => dev.shows.length === 1 && el('status').dataset.state === 'ok');
  for (let i = 0; i < 14; i++) { stroke([[100 + i * 20, 100], [110 + i * 20, 110]]); await shown(dev, 2 + i); await said(el, /partial update/); }
  el('debugsaw').value = 'grays look even';
  el('debugsaw').oninput();
  const r = el('report').value;
  assert.match(r, /^tiny-paper display report\ndevice: trmnl_x 1872x455, display firmware 0\.0\.0\+test/);
  assert.match(r, /device partial depths: 1,2,4; modes: full,partial,partial_all,inverse,passes/);
  assert.match(r, /tuning: gray_table=stock gray_partial=on gray_dark=0 gray_light=0 /);
  assert.match(r, /sketch: depth=auto \(4 bit in use\) mode=auto full_after=30 erase=auto/);
  assert.match(r, /what I saw: grays look even/);
  const status = r.split(/status lines[^\n]*:\n/)[1].split('\n');
  assert.equal(status.length, 12, 'the last 12');
  assert.ok(status.every((l) => /^\d\d:\d\d:\d\d Asked for /.test(l)), status[0]);
  assert.match(r, /last 12 status lines \(this firmware wrote no device log lines\):/, 'a firmware that wrote nothing says so');
  // the copy button hands the report to the clipboard
  await el('reportcopy').onclick();
  assert.equal(copied, r, 'the copy button puts the report on the clipboard');
  assert.equal(el('reportnote').textContent, 'Copied.');
});

test('Debug: each test pattern sends its own partial updates after a full refresh of the blank panel', async () => {
  const { dev, el } = await connected(456 + 100, XDEV);
  const run = async (name, partials) => {
    const before = dev.shows.length;
    await apply({ el }, { sketch: { test: name } });
    await until(`${name} sent`, () => el('debugresults').textContent.includes(`sketch.test: ${name} sent`) && dev.shows.length === before + 1 + partials);
    const mine = dev.shows.slice(before);
    assert.equal(mine[0].mode, 'full', `${name}: the blank panel first`);
    assert.deepEqual(new Set(mine.slice(1).map((s) => s.mode)), new Set(['partial']), `${name}: partial updates`);
    assert.ok(mine.slice(1).every((s) => s.bits === 4));
    assert.equal(el('report').value.includes(`test=${name}`), true);
  };
  await run('grays', 15); // 16 bars; the white one changes nothing
  await run('lines', 6);
  await run('box', 2);
  await apply({ el }, { sketch: { test: 'circles' } });
  await resultsHave({ el }, /sketch\.test: refused: grays, lines, box/);
});

test('Debug: the last pasted text is read back after a reload, and applying it again reproduces the try', async () => {
  const store = new Map();
  const storage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  const dev = fakeDisplay({ h: 458, ...XDEV });
  const p = open({ dev, storage });
  await p.page.connect();
  await until('connected', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
  const text = '{ "tuning": { "gray_dark": 4 } }';
  await apply(p, text);
  await resultsHave(p, /tuning\.gray_dark: applied: 4/);
  const dev2 = fakeDisplay({ h: 459, ...XDEV });
  const again = open({ dev: dev2, storage });
  assert.equal(again.el('debugjson').value, text, 'the text is back, not applied');
  await again.page.connect();
  await until('connected', () => dev2.shows.length === 1);
  assert.equal(dev2.knobs[2], 0, 'a reload applies nothing by itself');
  again.el('debugapply').onclick();
  await resultsHave(again, /tuning\.gray_dark: applied: 4/);
  assert.equal(dev2.knobs[2], 4);
});

test('the cursor circle is the brush diameter times the display scale, follows the pointer, and the toolbar dots show the same sizes', async () => {
  const { el, page, resize } = await connected(460);
  const canvas = el('canvas');
  const shownAt = (scale) => { canvas.getBoundingClientRect = () => ({ left: 10, top: 20, width: 800 * scale, height: 460 * scale }); resize(); };
  const cursor = el('cursor');
  const move = (x, y, type = 'mouse') => canvas.dispatch('pointermove', { ...pointer(x, y), pointerType: type });
  const sizes = [2, 5, 14]; // the brushes of an 800 wide panel
  assert.deepEqual(page.state.pen.brushes, sizes);
  for (const scale of [0.5, 0.375]) {
    shownAt(scale);
    sizes.forEach((px, i) => {
      assert.equal(el('brushes').children[i].children[0].style.width, `${px * scale}px`, `the dot of brush ${i} at scale ${scale}`);
      assert.equal(el('brushes').children[i].children[0].style.height, `${px * scale}px`);
    });
    for (let i = 0; i < 3; i++) {
      page.state.pen.size = i;
      move(110, 70);
      const d = sizes[i] * scale;
      assert.equal(cursor.hidden, false);
      assert.deepEqual([cursor.style.width, cursor.style.height], [`${d}px`, `${d}px`], `brush ${i} at scale ${scale}`);
      assert.equal(cursor.style.left, `${100 - d / 2}px`, 'centred on the pointer');
      assert.equal(cursor.style.top, `${50 - d / 2}px`);
      assert.equal(cursor.style.borderRadius, '50%');
    }
  }
  // a resize while the pointer rests: the circle follows the new scale without a move
  shownAt(0.25);
  assert.equal(cursor.style.width, `${14 * 0.25}px`);
  // black: a light ring around it; white: a dark one
  assert.match(cursor.style.boxShadow, /^0 0 0 1px rgb\(0 0 0\), 0 0 0 2px #fff$/);
  page.state.pen.color = 1;
  move(110, 70);
  assert.match(cursor.style.boxShadow, /2px #000$/);
  // the eraser: a square with a cross
  el('eraser').onclick();
  assert.equal(cursor.style.borderRadius, '0');
  assert.match(cursor.style.background, /linear-gradient/);
  el('eraser').onclick();
  // leaving the canvas hides it
  canvas.dispatch('pointerleave', {});
  assert.equal(cursor.hidden, true);
  // touch: nothing until the stroke starts, gone when it ends
  move(110, 70, 'touch');
  assert.equal(cursor.hidden, true);
  canvas.dispatch('pointerdown', { ...pointer(110, 70), pointerType: 'touch' });
  assert.equal(cursor.hidden, false);
  canvas.dispatch('pointerup', { ...pointer(110, 70), pointerType: 'touch' });
  assert.equal(cursor.hidden, true);
});

test('the toolbar brushes are dots with the word as label and tooltip, the chosen one marked', async () => {
  const { el } = await connected(461);
  const b = el('brushes').children;
  assert.deepEqual(b.map((x) => x.attrs['aria-label']), ['Small', 'Medium', 'Large']);
  assert.deepEqual(b.map((x) => x.title), ['Small', 'Medium', 'Large']);
  assert.deepEqual(b.map((x) => x.children[1].textContent), ['Small', 'Medium', 'Large']);
  assert.deepEqual(b.map((x) => x.attrs['aria-pressed']), ['false', 'true', 'false'], 'Medium is chosen at the start');
  b[2].onclick();
  assert.deepEqual(b.map((x) => x.attrs['aria-pressed']), ['false', 'false', 'true']);
});

// ---- the device's log lines (text outside frames): captured whole, attached to the update they belong to
const lineTexts = (p) => p.page.state.link.log.map((l) => l.text);

test('Device log: lines before, between and after frames are captured whole, a split line is joined, frames still parse', async () => {
  const dev = fakeDisplay({ h: 470, ...XDEV });
  dev.log[10] = { before: ['panel gpart depth=4 rows=100..380 ms=2567 dark=0 light=0 gray=stock steps=4 frames=9 chg=100 drv=90 rc=0', 'panel rows drive=32us n=380'], after: ['usb host seen: SOF'], split: true };
  dev.log[9] = { before: ['W something between chunks'] };
  const p = open({ dev });
  await p.page.connect();
  await until('connected', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
  p.stroke([[300, 200], [340, 240]]);
  await shown(dev, 2);
  await said(p.el, /partial update/);
  const lines = lineTexts(p);
  assert.ok(lines.includes('panel gpart depth=4 rows=100..380 ms=2567 dark=0 light=0 gray=stock steps=4 frames=9 chg=100 drv=90 rc=0'), 'the split line is whole');
  assert.ok(lines.includes('panel rows drive=32us n=380'));
  assert.ok(lines.includes('W something between chunks'));
  // the line after the answer is read with the next request, whole
  p.el('full').onclick();
  await shown(dev, 3);
  await until('the trailing line', () => lineTexts(p).includes('usb host seen: SOF'));
  assert.ok(p.page.state.link.log.every((l) => l.at > 0 && l.n > 0));
  assert.deepEqual(dev.shows.map((x) => x.mode).slice(0, 3), ['full', 'partial', 'full'], 'the frames parsed as before');
});

test('Device log: the report gives each update the lines that came between its request and its answer, indented, panel lines first', async () => {
  const dev = fakeDisplay({ h: 471, ...XDEV });
  const p = open({ dev });
  await p.page.connect();
  await until('connected', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
  dev.log[10] = { before: ['panel gpart depth=4 first'] };
  p.stroke([[300, 200], [340, 240]]);
  await shown(dev, 2); await said(p.el, /partial update/);
  dev.log[10] = { before: ['panel gpart depth=4 second', 'panel rows second'] };
  p.stroke([[500, 300], [540, 340]]);
  await shown(dev, 3); await until('second', () => p.page.state.history.length >= 3);
  const r = p.el('report').value;
  const at = r.split('\n');
  const first = at.findIndex((l) => /^\d\d:\d\d:\d\d Asked for a partial update/.test(l));
  assert.match(at[first + 1], /^    \| panel gpart depth=4 first$/);
  assert.match(at[first + 2], /^\d\d:\d\d:\d\d Asked for a partial update/, 'the next update starts after the first one\'s lines');
  assert.match(at[first + 3], /^    \| panel gpart depth=4 second$/);
  assert.match(at[first + 4], /^    \| panel rows second$/);
  assert.match(r, /with the device log lines of each update, indented:/);
  // the checkbox leaves them out
  p.el('reportlog').checked = false;
  p.el('reportlog').onchange();
  assert.ok(!p.el('report').value.includes('    | '));
});

test('Device log: at most 12 updates with 8 lines each, panel lines kept first, and the cut is said', async () => {
  const dev = fakeDisplay({ h: 472, ...XDEV });
  const p = open({ dev });
  await p.page.connect();
  await until('connected', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
  dev.log[10] = { before: [...Array.from({ length: 7 }, (_, i) => `noise ${i}`), 'panel gpart depth=4 the one that matters', ...Array.from({ length: 4 }, (_, i) => `more ${i}`)] };
  p.stroke([[300, 200], [340, 240]]);
  await shown(dev, 2); await until('done', () => p.page.state.history.length >= 2);
  const r = p.el('report').value;
  assert.match(r, /\| panel gpart depth=4 the one that matters/);
  assert.match(r, /\| \(4 more device log lines cut; at most 8 are kept\)/);
  assert.equal(r.split('\n').filter((l) => l.startsWith('    | ') && !l.includes('cut;')).length, 8);
  assert.ok(!r.includes('more 3'), 'the last lines are what is cut');
  // the box shows the last 40
  dev.log[10] = { before: Array.from({ length: 60 }, (_, i) => `line ${i}`) };
  p.stroke([[500, 300], [540, 340]]);
  await shown(dev, 3); await until('done', () => p.page.state.history.length >= 3);
  const box = p.el('devicelog').textContent.split('\n');
  assert.equal(box.length, 40);
  assert.match(box.at(-1), /line 59$/);
  assert.ok(p.page.state.link.log.length <= 200);
});

test('Device log: a firmware that writes nothing says so in the box and in the report', async () => {
  const { dev, el, stroke, page } = await connected(473, XDEV);
  stroke([[300, 200], [340, 240]]);
  await shown(dev, 2); await until('done', () => page.state.history.length >= 2);
  assert.equal(el('devicelogwhy').textContent, 'This firmware writes no log lines.');
  assert.match(el('report').value, /\(this firmware wrote no device log lines\)/);
});

test('Grays: the page starts at 1 bit with two swatches on a gray capable device; the switch gives 16 swatches and 4 bit sends; switching back is one full refresh at 1 bit', async () => {
  const store = new Map();
  const storage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  const dev = fakeDisplay({ h: 480 + 1, ...XDEV });
  const p = open({ dev, storage });
  const { el, stroke } = p;
  await p.page.connect();
  await until('the whole drawing', () => dev.shows.length === 1 && el('status').dataset.state === 'ok');
  assert.equal(dev.shows[0].bits, 1, 'the default is 1 bit');
  assert.deepEqual(el('swatches').children.map((b) => b.title), ['black', 'white']);
  assert.equal(el('note').hidden, true, 'no "cannot show grays" note: the switch is there');
  assert.equal(el('graysrow').hidden, false);
  assert.equal(el('grays').checked, false);
  stroke([[300, 200], [340, 240]]);
  await shown(dev, 2);
  assert.equal(dev.shows[1].bits, 1);
  // grays on: 16 swatches, one full refresh at 4 bit, then strokes at 4 bit
  el('grays').checked = true;
  el('grays').onchange();
  await shown(dev, 3);
  assert.deepEqual([dev.shows[2].mode, dev.shows[2].bits], ['full', 4]);
  await said(el, /Switched to grays: the canvas is sent with one full refresh/);
  assert.equal(el('swatches').children.length, 16);
  assert.equal(store.get('tp-sketch-grays-trmnl_x'), '1', 'remembered for this board');
  p.page.state.pen.color = 6;
  stroke([[500, 300], [540, 340]]);
  await shown(dev, 4);
  assert.deepEqual([dev.shows[3].mode, dev.shows[3].bits], ['partial', 4]);
  // back: the canvas thresholded, one full refresh at 1 bit, nothing else
  el('grays').checked = false;
  el('grays').onchange();
  await shown(dev, 5);
  assert.deepEqual([dev.shows[4].mode, dev.shows[4].bits], ['full', 1]);
  await said(el, /Switched to black and white: the canvas was thresholded and is sent with one full refresh/);
  assert.equal(el('swatches').children.length, 2);
  assert.equal(store.get('tp-sketch-grays-trmnl_x'), '0');
  await sleep(500);
  assert.equal(dev.shows.length, 5, 'one full refresh, no error, nothing more');
  assert.ok(new Set(levels(dev.pngs[4])).size <= 2 && [...levels(dev.pngs[4])].every((v) => v === 0 || v === 1), 'only black and white went out');
  assert.ok(levels(dev.pngs[4]).includes(0), 'the black strokes are still there');
  // a reload remembers the choice
  store.set('tp-sketch-grays-trmnl_x', '1');
  const dev2 = fakeDisplay({ h: 480 + 2, ...XDEV });
  const again = open({ dev: dev2, storage });
  await again.page.connect();
  await until('connected', () => dev2.shows.length === 1 && again.el('status').dataset.state === 'ok');
  assert.equal(dev2.shows[0].bits, 4);
  assert.equal(again.el('grays').checked, true);
  // a black and white device has no switch; the Debug depth still overrides
  const bw = await connected(480 + 3, { board: 'trmnl_og' });
  assert.equal(bw.el('graysrow').hidden, true);
  await apply({ el }, { sketch: { depth: 2 } });
  await until('depth 2', () => el('swatches').children.length === 4);
});

// ---- a device that keeps another picture or depth: the page never sends it a rectangle first
test('Connect to a device that keeps another frame or depth: the first send is the whole canvas, the second a rectangle, no refusal', async () => {
  for (const [h, keeps, fingerprint] of [[490, 4, '0123456789abcdef'], [491, 1, 'fedcba9876543210'], [492, null, undefined]]) {
    const dev = fakeDisplay({ h, ...XDEV, ...(fingerprint ? { fingerprint } : {}) });
    dev.keeps = keeps; // the frame the device kept from before (a 4 bit test pattern, another 1 bit picture, none)
    const p = open({ dev });
    await p.page.connect();
    await until('the first send', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
    assert.deepEqual([dev.shows[0].mode, dev.shows[0].x, dev.shows[0].w, dev.shows[0].h, dev.shows[0].bits], ['full', undefined, 1872, h, 1], `kept ${keeps}: the whole canvas first`);
    p.stroke([[300, 200], [340, 240]]);
    await shown(dev, 2);
    assert.deepEqual([dev.shows[1].mode, dev.shows[1].x !== undefined, dev.shows[1].bits], ['partial', true, 1], 'then a rectangle');
    await said(p.el, /partial update of/);
    assert.deepEqual(dev.refusals, [], 'the device refused nothing');
  }
});

test('Connect to a device that keeps this very drawing: nothing is sent, the first stroke is a rectangle', async () => {
  const h = 493;
  const blank = fingerprintOf(new Uint8Array(1872 * h).fill(1), 1872, h, 1);
  const dev = fakeDisplay({ h, ...XDEV, fingerprint: blank });
  dev.keeps = 1;
  const p = open({ dev });
  await p.page.connect();
  await until('connected', () => /already keeps this drawing/.test(p.el('status').textContent));
  await sleep(300);
  assert.equal(dev.shows.length, 0, 'the device keeps the same frame: no whole-canvas send');
  p.stroke([[300, 200], [340, 240]]);
  await shown(dev, 1);
  assert.equal(dev.shows[0].mode, 'partial');
  assert.deepEqual(dev.refusals, []);
});

test('A rectangle the device refuses for its depth is said in words, the frame is forgotten and the whole canvas goes out once', async () => {
  const dev = fakeDisplay({ h: 494, ...XDEV });
  const p = open({ dev });
  await p.page.connect();
  await until('first send', () => dev.shows.length === 1 && p.el('status').dataset.state === 'ok');
  dev.keeps = 4; // another picture appears on the device behind the page's back
  p.stroke([[300, 200], [340, 240]]);
  await until('the refusal', () => /Not shown/.test(p.el('status').textContent));
  assert.match(p.el('status').textContent, /the picture shown has 4 bit and the rectangle 1 bit/);
  assert.match(p.el('status').textContent, /the next update is the whole drawing/);
  p.stroke([[500, 300], [540, 340]]); // the next update: the whole drawing, once, then rectangles again
  await shown(dev, 2);
  assert.deepEqual([dev.shows[1].mode, dev.shows[1].x, dev.shows[1].w], ['full', undefined, 1872]);
  await said(p.el, /Asked for a full refresh/);
  p.stroke([[600, 200], [640, 240]]);
  await shown(dev, 3);
  assert.equal(dev.shows[2].mode, 'partial');
  assert.equal(dev.refusals.length, 1, 'one refusal, then none');
});

test('The fingerprint of the page is the one of the spec: the vectors', async () => {
  const vectors = JSON.parse(readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'docs/spec/vectors/usb-display.json'), 'utf8')).fingerprints;
  let n = 0;
  for (const v of vectors) {
    if (v.color) continue;
    const png = Uint8Array.from(Buffer.from(v.png, 'hex'));
    const w = new DataView(png.buffer).getUint32(16), h = new DataView(png.buffer).getUint32(20);
    assert.equal(fingerprintOf(Uint8Array.from(levels(png)), w, h, v.bits), v.fingerprint, `${v.bits} bit`);
    n++;
  }
  assert.ok(n >= 6);
});
