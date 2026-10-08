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
function open({ dev = null, search = '', serial = true } = {}) {
  const document = fakeDocument(HTML);
  const listeners = {};
  const navigator = serial ? { serial: {
    requestPort: async () => { if (!dev) throw new Error('nothing picked'); return dev.port; },
    addEventListener: (type, f) => { listeners[type] = f; },
  } } : {};
  const page = startSketchPage({ document, navigator, location: { search }, ImageData: FakeImageData });
  const el = (id) => document.getElementById(id);
  const stroke = (points) => {
    el('canvas').dispatch('pointerdown', pointer(...points[0]));
    for (const p of points.slice(1)) el('canvas').dispatch('pointermove', pointer(...p));
    el('canvas').dispatch('pointerup', pointer(...points.at(-1)));
  };
  return { page, el, stroke, listeners };
}
// each test its own panel height: the page keeps one drawing per panel shape while it is loaded
async function connected(h, over = {}) {
  const dev = fakeDisplay({ h, ...over });
  const p = open({ dev });
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
  assert.deepEqual(el('brushes').children.map((b) => b.textContent), ['Small', 'Medium', 'Large']);
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
  // undo is a change too: it goes out as a partial show of the same area
  el('undo').onclick();
  await until('the undo', () => dev.shows.length === 3);
  assert.deepEqual([dev.shows[2].mode, dev.shows[2].x, dev.shows[2].w], ['partial', s.x, s.w]);
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
  assert.match(el('status').textContent, /Not shown: The rectangle is outside the panel or misaligned: rect\. Draw on or press Full refresh/);
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

  const x = open({ search: '?mock=x', serial: false });
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
