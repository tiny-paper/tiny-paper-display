// SPDX-License-Identifier: MIT
// The Sketch page (sketch.html): its buttons, canvas and status line, and the cable. What is drawn, what is sent and
// when is sketch.js, the main repository's display modules (also its web app's) as one file; the link over Web Serial
// is the one the Bluetooth setup uses. `?mock=x|og|picpak`: no device, a pretend one answers.
// Every failure ends as plain words on the page.
import { DisplayLink } from './bt-setup.js';
import { BRUSH_NAMES, DisplayClient, FULL_AFTER, MOCK_PANELS, MockDisplayDevice, SketchPen, displayHello, sketchSession } from './sketch.js';

const HELLO_MS = 3000;
// a full refresh of the largest panel takes several seconds; the device answers when the panel is done
const SHOW_MS = 120000;
const BOARD_TITLES = { trmnl_x: 'TRMNL X', trmnl_og: 'TRMNL OG', picpak: 'PicPak' };

/** A pretend device as a Web Serial port: the page talks to it exactly as to a real one. */
export function mockPort(device) {
  const port = {
    readable: null,
    writable: null,
    async open() {
      port.readable = new ReadableStream({
        start(c) { device.onOutput((b) => { try { c.enqueue(b); } catch { /* closed */ } }); },
      });
      port.writable = new WritableStream({ write(chunk) { device.write(chunk); } });
    },
    async close() { device.onOutput(() => {}); port.readable = null; port.writable = null; },
  };
  return port;
}

/**
 * Start the page in `env` ({ document, navigator, location, ImageData }): returns { ready, connect, disconnect, state }.
 * `ready` resolves when the preview device, if asked for, is attached.
 */
export function startSketchPage(env) {
  const doc = env.document;
  const $ = (id) => {
    const el = doc.getElementById(id);
    if (!el) throw new Error(`sketch.html has no #${id}`);
    return el;
  };
  const serial = env.navigator && 'serial' in env.navigator ? env.navigator.serial : null;
  const state = { link: null, port: null, hello: null, sender: null, pen: null, preview: null };
  const canvas = $('canvas');

  const say = (s) => {
    $('status').textContent = s.text;
    $('status').className = s.state === 'error' ? 'warn' : '';
    $('status').dataset.state = s.state;
  };
  const device = (text, cls) => { $('device').textContent = text; $('device').className = cls || 'mut'; };
  const paint = (r) => {
    if (r.w < 1 || r.h < 1 || !state.sender) return;
    const ctx = canvas.getContext('2d');
    if (ctx) ctx.putImageData(new env.ImageData(state.sender.model.rgba(r, state.sender.ink), r.w, r.h), r.x, r.y);
  };
  const pressed = () => {
    const pen = state.pen;
    [...$('brushes').children].forEach((b, i) => b.setAttribute('aria-pressed', String(pen.size === i)));
    [...$('swatches').children].forEach((b) => b.setAttribute('aria-pressed', String(!pen.eraser && String(pen.color) === b.dataset.value)));
    $('eraser').setAttribute('aria-pressed', String(pen.eraser));
    $('undo').disabled = !pen.canUndo;
  };
  const button = (text, onclick) => {
    const b = doc.createElement('button');
    b.textContent = text;
    b.onclick = onclick;
    return b;
  };

  /** The tools and the canvas for this device's panel; its drawing is kept while the page is open. */
  function show(hello, client) {
    const sender = sketchSession(hello);
    const pen = new SketchPen(sender);
    state.hello = hello; state.sender = sender; state.pen = pen;
    $('brushes').replaceChildren(...BRUSH_NAMES.map((name, i) => button(name, () => { pen.size = i; pressed(); })));
    $('swatches').replaceChildren(...sender.ink.swatches.map((s) => {
      const b = button('', () => { pen.color = s.value; pen.eraser = false; pressed(); });
      b.className = 'swatch';
      b.title = s.name;
      b.setAttribute('aria-label', s.name);
      b.dataset.value = String(s.value);
      b.style.background = `rgb(${s.rgb.join(' ')})`;
      return b;
    }));
    $('note').textContent = sender.ink.note;
    $('note').hidden = !sender.ink.note;
    $('fullafterrow').hidden = !sender.ink.partial;
    $('fullafter').value = String(FULL_AFTER);
    sender.fullAfter = FULL_AFTER;
    canvas.width = sender.model.w;
    canvas.height = sender.model.h;
    sender.model.onPaint = paint;
    paint(sender.model.whole);
    sender.onStatus = say;
    say(sender.status);
    $('pad').hidden = false;
    $('full').disabled = false;
    pressed();
    sender.attach(client, hello);
  }

  /** The device is gone (or another one comes): the drawing stays on the page. */
  async function disconnect(text) {
    const link = state.link;
    state.link = null; state.port = null; state.preview = null;
    if (state.sender) { state.sender.attach(null); $('full').disabled = true; }
    $('connect').textContent = 'Connect';
    $('preview').hidden = true;
    if (text) device(text, 'warn');
    if (link) await link.close();
  }

  /** Open the port, ask hello and start. A device that is not the display firmware: say how to install it. */
  async function attach(port, preview) {
    await disconnect('');
    $('install').hidden = true;
    const link = new DisplayLink(port, { timeoutMs: HELLO_MS });
    await link.open();
    let hello;
    try {
      hello = displayHello(await link.request(1, { proto: 1, app: 'tiny-paper-display-site' }));
    } catch (e) {
      await link.close();
      $('installwhy').textContent = 'This device does not answer as the display firmware.';
      $('install').hidden = false;
      throw e;
    }
    state.link = link; state.port = port; state.preview = preview;
    const title = BOARD_TITLES[hello.board] ?? hello.board;
    device(`${title}, ${hello.w} x ${hello.h}, display firmware ${hello.fw}.`, 'ok');
    $('connect').textContent = preview ? 'Connect a device' : 'Disconnect';
    $('preview').textContent = preview ? `Preview: no device is attached. A pretend ${MOCK_PANELS[preview]} answers, so nothing reaches a panel.` : '';
    $('preview').hidden = !preview;
    show(hello, new DisplayClient({ requestRaw: (cmd, req) => link.requestRaw(cmd, req, { timeoutMs: SHOW_MS }) }));
  }

  async function connect() {
    if (state.link && !state.preview) { await disconnect('Disconnected. The drawing is kept: connect again to send it.'); return; }
    $('connect').disabled = true;
    try {
      let port;
      try {
        port = await serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
      } catch {
        throw new Error('No port was chosen. Press Connect and pick the device\'s port.');
      }
      device('Asking the device...');
      await attach(port, null);
    } catch (e) {
      device(e && e.message ? e.message : String(e), 'warn');
    }
    $('connect').disabled = false;
  }

  const at = (e) => {
    const b = canvas.getBoundingClientRect();
    return { x: (e.clientX - b.left) * canvas.width / b.width, y: (e.clientY - b.top) * canvas.height / b.height };
  };
  canvas.addEventListener('pointerdown', (e) => {
    if (!state.pen || (e.pointerType === 'mouse' && e.button !== 0)) return;
    if (!state.pen.down(e.pointerId, at(e))) return;
    e.preventDefault();
    if (canvas.setPointerCapture) canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!state.pen) return;
    const more = e.getCoalescedEvents ? e.getCoalescedEvents() : [];
    state.pen.move(e.pointerId, (more.length ? more : [e]).map(at));
  });
  const up = (e) => { if (state.pen && state.pen.up(e.pointerId)) pressed(); };
  canvas.addEventListener('pointerup', up);
  canvas.addEventListener('pointercancel', up);
  $('eraser').onclick = () => { state.pen.eraser = !state.pen.eraser; pressed(); };
  $('undo').onclick = () => { state.pen.undo(); pressed(); };
  $('clear').onclick = () => { state.pen.clear(); pressed(); };
  $('full').onclick = () => state.sender.fullRefresh();
  $('fullafter').onchange = () => {
    const n = Number($('fullafter').value);
    state.sender.fullAfter = $('fullafter').value !== '' && Number.isInteger(n) && n >= 0 && n <= 250 ? n : FULL_AFTER;
  };
  $('connect').onclick = connect;
  $('connect').disabled = !serial;
  $('nosupport').hidden = !!serial;
  if (serial && serial.addEventListener) {
    serial.addEventListener('disconnect', (e) => {
      if (state.port && e.target === state.port) void disconnect('The device was unplugged. The drawing is kept: plug it in and connect again.');
    });
  }

  const mock = new URLSearchParams(env.location.search).get('mock');
  let ready = Promise.resolve();
  if (mock !== null && !Object.hasOwn(MOCK_PANELS, mock)) {
    $('preview').textContent = `There is no preview panel "${mock}": use ?mock=x, ?mock=og or ?mock=picpak.`;
    $('preview').hidden = false;
  } else if (mock !== null) {
    ready = attach(mockPort(new MockDisplayDevice(mock)), mock).catch((e) => device(e && e.message ? e.message : String(e), 'warn'));
  }
  return { ready, connect, disconnect, state };
}

if (typeof document !== 'undefined') startSketchPage({ document, navigator, location, ImageData });
