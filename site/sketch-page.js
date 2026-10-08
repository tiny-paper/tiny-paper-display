// SPDX-License-Identifier: MIT
// The Sketch page (sketch.html): its buttons, canvas and status line, and the cable. What is drawn, what is sent and
// when is sketch.js, the main repository's display modules (also its web app's) as one file; the link over Web Serial
// is the one the Bluetooth setup uses. `?mock=x|og|picpak`: no device, a pretend one answers.
// Every failure ends as plain words on the page.
import { DisplayLink } from './bt-setup.js';
import {
  BRUSH_NAMES, DEBUG_DEFAULTS, DisplayClient, FULL_AFTER, MOCK_PANELS, MockDisplayDevice, SketchPen, checkSketch, displayHello, parseDebug,
  reportText, sketchInk, sketchSession, testOf, testPlan, tuningRequest, tuningResults,
} from './sketch.js';

const HELLO_MS = 3000;
// a full refresh of the largest panel takes several seconds; the device answers when the panel is done
const SHOW_MS = 120000;
const LOG_SHOWN = 40;
const DEBUG_KEY = 'tp-sketch-debug-text';
const BOARD_TITLES = { trmnl_x: 'TRMNL X', trmnl_og: 'TRMNL OG', picpak: 'PicPak' };

/** What the device's hello says about partial updates, in words: "Partial updates at: 1, 2 and 4 bit (16 grays)". */
export function partialLine(hello) {
  const d = [...hello.partial_depths].sort((a, b) => a - b);
  if (!d.length) return 'Partial updates at: none (full refreshes only)';
  const list = d.length === 1 ? `${d[0]}` : `${d.slice(0, -1).join(', ')} and ${d.at(-1)}`;
  return `Partial updates at: ${list} bit${d.at(-1) > 1 ? ` (${2 ** d.at(-1)} grays)` : ''}`;
}

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
 * Start the page in `env` ({ document, navigator, location, ImageData, localStorage? }): returns { ready, connect, disconnect, state }.
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
  const state = { link: null, port: null, hello: null, sender: null, pen: null, preview: null, client: null,
    dbg: { ...DEBUG_DEFAULTS }, history: [], busy: false, test: null, mark: null, grays: false };
  const store = {
    get: (k) => { try { return env.localStorage?.getItem(k) ?? null; } catch { return null; } },
    set: (k, v) => { try { env.localStorage?.setItem(k, v); } catch { /* not remembered */ } },
  };
  const canvas = $('canvas');

  const say = (s) => {
    $('status').textContent = s.text;
    $('status').className = s.state === 'error' ? 'warn' : '';
    $('status').dataset.state = s.state;
    if (s.state === 'sending' && !state.mark) state.mark = { anchored: false, n: state.link ? state.link.logCount : 0 };
    if (s.state === 'ok' || s.state === 'error') {
      const mark = state.mark;
      state.mark = null;
      const lines = state.link && mark ? state.link.log.filter((l) => l.n > mark.n).map((l) => l.text) : [];
      state.history.push({ at: new Date().toTimeString().slice(0, 8), text: s.text, lines });
      if (state.history.length > 12) state.history.shift();
    }
    logBox();
    report();
  };
  const report = () => {
    $('report').value = reportText({ hello: state.hello, sketch: state.dbg, depthUsed: state.sender ? state.sender.ink.depth : null,
      saw: $('debugsaw').value, history: state.history, test: state.test, withLog: $('reportlog').checked !== false,
      logSeen: !!(state.link && state.link.logCount) });
  };
  /** The Device log box: the last lines the device wrote between and around its frames. */
  const logBox = () => {
    const link = state.link;
    const stamp = (t) => new Date(t).toTimeString().slice(0, 8);
    $('devicelog').textContent = link ? link.log.slice(-LOG_SHOWN).map((l) => `${stamp(l.at)} ${l.text}`).join('\n') : '';
    $('devicelogwhy').textContent = !link ? '' : link.logCount ? `The last ${Math.min(LOG_SHOWN, link.log.length)} lines the device wrote (${link.logCount} so far).`
      : state.history.length ? 'This firmware writes no log lines.' : 'No log lines yet: they come with the answers to updates.';
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
    place();
    $('eraser').setAttribute('aria-pressed', String(pen.eraser));
    $('undo').disabled = !pen.canUndo;
  };
  /** Panel pixels to screen pixels: the canvas is shown at its own width, usually smaller than the panel's. */
  const scale = () => { const b = canvas.getBoundingClientRect(); return canvas.width ? b.width / canvas.width : 1; };
  const cursor = $('cursor');
  const mark = { x: 0, y: 0, on: false };
  /** The brush dots of the toolbar and the cursor circle, at the display scale: what a tap leaves is what they show. */
  function sizes() {
    const s = scale();
    if (state.pen) [...$('brushes').children].forEach((b, i) => {
      const d = state.pen.diameterOf(i) * s;
      b.children[0].style.width = `${d}px`;
      b.children[0].style.height = `${d}px`;
    });
    place();
  }
  function place() {
    if (!mark.on || !state.pen) { cursor.hidden = true; return; }
    const d = state.pen.diameter * scale();
    const sw = state.sender.ink.swatches.find((x) => x.value === state.pen.color);
    const rgb = sw ? sw.rgb : [0, 0, 0];
    const light = rgb[0] * 0.299 + rgb[1] * 0.587 + rgb[2] * 0.114 > 128;
    const eraser = state.pen.eraser;
    const inner = eraser ? '#000' : `rgb(${rgb.join(' ')})`;
    const outer = eraser ? '#fff' : light ? '#000' : '#fff';
    Object.assign(cursor.style, {
      width: `${d}px`, height: `${d}px`, left: `${mark.x - d / 2}px`, top: `${mark.y - d / 2}px`,
      borderRadius: eraser ? '0' : '50%', boxShadow: `0 0 0 1px ${inner}, 0 0 0 2px ${outer}`,
      background: eraser ? 'linear-gradient(to top right, transparent calc(50% - 0.5px), #888 calc(50% - 0.5px) calc(50% + 0.5px), transparent calc(50% + 0.5px)), linear-gradient(to bottom right, transparent calc(50% - 0.5px), #888 calc(50% - 0.5px) calc(50% + 0.5px), transparent calc(50% + 0.5px))' : 'none',
    });
    cursor.hidden = false;
  }
  /** The pointer is over the canvas at e: the cursor follows it (a touch shows it only while a stroke is drawn). */
  function follow(e, stroke) {
    if (e.pointerType === 'touch' && !stroke) return;
    const b = canvas.getBoundingClientRect();
    mark.x = e.clientX - b.left; mark.y = e.clientY - b.top; mark.on = true;
    $('canvaswrap').className = 'pen';
    place();
  }
  const button = (text, onclick) => {
    const b = doc.createElement('button');
    b.textContent = text;
    b.onclick = onclick;
    return b;
  };

  /** The tools and the canvas for this device's panel; its drawing is kept while the page is open. */
  /** The depth strokes go out at: the Debug depth, else 1 bit where the device does partial updates at it (the best looking and fastest on the X), grays when asked for. */
  const depthFor = (hello) => {
    if (state.dbg.depth !== null) return state.dbg.depth;
    const d = hello.partial_depths || [];
    return state.grays && d.some((x) => x > 1) ? Math.max(...d) : d.includes(1) ? 1 : undefined;
  };
  function show(hello, client, note = '') {
    const prev = state.sender;
    const sender = sketchSession(hello, depthFor(hello));
    if (prev && prev !== sender && prev.model.w === sender.model.w && prev.model.h === sender.model.h) sender.model.adopt(prev.model); // another depth: the same drawing
    const gray = (hello.partial_depths || []).some((x) => x > 1) && !hello.colors;
    $('graysrow').hidden = !gray;
    $('grays').checked = state.grays;
    const pen = new SketchPen(sender);
    state.hello = hello; state.sender = sender; state.pen = pen; state.client = client;
    sender.strokeMode = state.dbg.mode; sender.erase = state.dbg.erase;
    $('debugnone').hidden = !!hello.tuning;
    $('brushes').replaceChildren(...BRUSH_NAMES.map((name, i) => {
      const b = button('', () => { pen.size = i; pressed(); });
      const dot = doc.createElement('span');
      dot.className = 'dot';
      const word = doc.createElement('span');
      word.className = 'sr';
      word.textContent = name;
      b.replaceChildren(dot, word);
      b.title = name;
      b.setAttribute('aria-label', name);
      return b;
    }));
    sizes();
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
    $('fullafter').value = String(state.dbg.full_after);
    sender.fullAfter = state.dbg.full_after;
    canvas.width = sender.model.w;
    canvas.height = sender.model.h;
    sizes();
    sender.model.onPaint = paint;
    paint(sender.model.whole);
    sender.onStatus = say;
    say(sender.status);
    $('pad').hidden = false;
    sizes(); // the pad is laid out now: the dots take the display scale
    $('full').disabled = false;
    pressed();
    sender.attach(client, hello);
    if (note && sender.ink.partial) sender.fullWith(note);
  }

  /** The device is gone (or another one comes): the drawing stays on the page. */
  async function disconnect(text) {
    const link = state.link;
    state.link = null; state.port = null; state.preview = null;
    if (state.sender) { state.sender.attach(null); $('full').disabled = true; }
    $('connect').textContent = 'Connect';
    $('preview').hidden = true;
    $('partial').hidden = true;
    if (text) device(text, 'warn');
    if (link) await link.close();
  }

  /** Open the port, ask hello and start. A device that is not the display firmware: say how to install it. */
  async function attach(port, preview) {
    state.grays = false;
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
    state.grays = store.get(`tp-sketch-grays-${hello.board}`) === '1';
    link.onRequest = (n) => { if (state.mark && !state.mark.anchored) { state.mark.n = n; state.mark.anchored = true; } };
    link.onLog = () => { logBox(); report(); };
    const title = BOARD_TITLES[hello.board] ?? hello.board;
    device(`${title}, ${hello.w} x ${hello.h}, display firmware ${hello.fw}.`, 'ok');
    $('partial').textContent = partialLine(hello);
    $('partial').hidden = false;
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
    follow(e, true);
    e.preventDefault();
    if (canvas.setPointerCapture) canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!state.pen) return;
    const more = e.getCoalescedEvents ? e.getCoalescedEvents() : [];
    state.pen.move(e.pointerId, (more.length ? more : [e]).map(at));
    follow(e, state.pen.drawing);
  });
  canvas.addEventListener('pointerleave', () => { mark.on = false; $('canvaswrap').className = ''; place(); });
  if (env.onResize) env.onResize(sizes);
  const up = (e) => {
    if (state.pen && state.pen.up(e.pointerId)) pressed();
    if (e.pointerType === 'touch') { mark.on = false; place(); }
  };
  canvas.addEventListener('pointerup', up);
  canvas.addEventListener('pointercancel', up);
  $('eraser').onclick = () => { state.pen.eraser = !state.pen.eraser; pressed(); place(); };
  $('undo').onclick = () => { state.pen.undo(); pressed(); };
  $('clear').onclick = () => { state.pen.clear(); pressed(); };
  $('full').onclick = () => state.sender.fullRefresh();
  $('fullafter').onchange = () => {
    const n = Number($('fullafter').value);
    state.sender.fullAfter = $('fullafter').value !== '' && Number.isInteger(n) && n >= 0 && n <= 250 ? n : FULL_AFTER;
  };
  /** The Debug section: apply a pasted configuration, one line per key. */
  const lines = (list) => { $('debugresults').textContent = list.map((r) => `${r.key}: ${r.text}`).join('\n'); report(); };
  async function runTest(name) {
    const s = state.sender;
    if (!s.ink.partial) return [{ key: 'sketch.test', applied: false, text: 'refused: this device has no partial updates' }];
    state.busy = true; state.test = name; $('debugapply').disabled = true;
    try {
      s.model.clear(); s.fullRefresh(); await s.sendNow();
      const whole = name === 'grays_full';
      for (const step of testPlan(name, s.model.w, s.model.h, s.ink.paper)) {
        if (step.rect) s.model.fillRect(step.rect, step.value);
        else { s.model.begin(); s.model.line(...step.line); s.model.end(); }
        if (!whole) await s.sendNow();
      }
      if (whole) { s.fullRefresh(); await s.sendNow(); }
    } finally { state.busy = false; $('debugapply').disabled = false; }
    return [{ key: 'sketch.test', applied: true, text: `${name} sent${name === 'grays_full' ? ' (one full refresh)' : ''}` }];
  }
  function takeSketch(next) {
    const before = state.sender.ink.depth;
    state.dbg = next;
    if (sketchInk(state.hello, depthFor(state.hello)).depth !== before) show(state.hello, state.client); // another depth: the same drawing, sent whole
    else { state.sender.strokeMode = next.mode; state.sender.erase = next.erase; state.sender.fullAfter = next.full_after; $('fullafter').value = String(next.full_after); }
  }
  async function applyDebug() {
    const text = $('debugjson').value;
    store.set(DEBUG_KEY, text);
    const p = parseDebug(text);
    if (!p.ok) { lines([{ key: 'text', text: p.error }]); return; }
    if (!state.sender || !state.client) { lines([{ key: 'device', text: 'no device connected: nothing applied' }]); return; }
    const out = p.extra.map((k) => ({ key: k, applied: false, text: 'refused: no such part; use tuning and sketch' }));
    if (p.tuning !== undefined) {
      const names = p.tuning === 'defaults' ? [] : Object.keys(p.tuning);
      if (!state.hello.tuning) {
        out.push(...(names.length ? names : ['tuning']).map((k) => ({ key: k === 'tuning' ? k : `tuning.${k}`, applied: false, text: 'refused: this device has no panel tuning' })));
      } else {
        const { req, results } = tuningRequest(p.tuning);
        out.push(...results);
        if (req === 'defaults' || Object.keys(req).length) {
          try {
            const r = await state.client.request(12, { tuning: req });
            if (r.tuning) state.hello.tuning = r.tuning;
            if (req === 'defaults') out.push({ key: 'tuning', applied: !r.refused, text: r.refused ? `refused: ${JSON.stringify(r.refused)}` : 'defaults restored' });
            else out.push(...tuningResults(Object.keys(req), r.refused, r.tuning));
          } catch (e) { out.push({ key: 'tuning', applied: false, text: `refused: ${e && e.message ? e.message : String(e)}` }); }
        }
      }
    }
    const { next, results } = checkSketch(p.sketch, state.hello, state.dbg);
    out.push(...results);
    takeSketch(next);
    lines(out);
    const t = testOf(p.sketch);
    if (t) {
      try { out.push(...await runTest(t)); } catch (e) { out.push({ key: 'sketch.test', applied: false, text: `failed: ${e && e.message ? e.message : String(e)}` }); }
      lines(out);
    }
  }
  $('debugapply').onclick = () => { if (!state.busy) void applyDebug(); };
  $('debugreset').onclick = async () => {
    if (state.busy || !state.client) { lines([{ key: 'device', text: 'no device connected: nothing reset' }]); return; }
    const out = [];
    if (state.hello.tuning) {
      try {
        const r = await state.client.request(12, { tuning: 'defaults' });
        if (r.tuning) state.hello.tuning = r.tuning;
        out.push({ key: 'tuning', text: 'defaults restored' });
      } catch (e) { out.push({ key: 'tuning', text: `refused: ${e && e.message ? e.message : String(e)}` }); }
    }
    takeSketch({ ...DEBUG_DEFAULTS });
    out.push({ key: 'sketch', text: 'defaults restored' });
    state.test = null;
    lines(out);
  };
  $('grays').onchange = () => {
    if (!state.hello || !state.client || state.busy) return;
    state.grays = $('grays').checked;
    store.set(`tp-sketch-grays-${state.hello.board}`, state.grays ? '1' : '0');
    show(state.hello, state.client, state.grays
      ? 'Switched to grays: the canvas is sent with one full refresh, because the device keeps one depth at a time.'
      : 'Switched to black and white: the canvas was thresholded and is sent with one full refresh, because the device keeps one depth at a time.');
  };
  $('debugsaw').oninput = report;
  $('reportlog').onchange = report;
  $('reportcopy').onclick = async () => {
    const text = $('report').value;
    try { await env.navigator.clipboard.writeText(text); $('reportnote').textContent = 'Copied.'; } catch {
      try { $('report').select?.(); $('reportnote').textContent = env.document.execCommand?.('copy') ? 'Copied.' : 'Select the text above and copy it.'; } catch { $('reportnote').textContent = 'Select the text above and copy it.'; }
    }
  };
  $('debugjson').value = store.get(DEBUG_KEY) ?? '';
  report();
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

if (typeof document !== 'undefined') {
  let storage = null;
  try { storage = localStorage; } catch { /* blocked: the Debug text is not remembered */ }
  startSketchPage({ document, navigator, location, ImageData, localStorage: storage, onResize: (f) => addEventListener('resize', f) });
}
