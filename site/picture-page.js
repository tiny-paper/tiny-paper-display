// SPDX-License-Identifier: MIT
// The Picture page (picture.html): its controls, preview canvas, status line and the cable. What is prepared (scaling,
// dithering), the test pictures, the Compare plan and the report text are picture.js, made from site/src/picture; the link
// over Web Serial is the one the Sketch page uses. `?mock=x|og|picpak`: no device, a pretend one answers.
// The preview canvas holds exactly the levels that are sent. Every failure ends as plain words on the page.
import { DisplayLink } from './bt-setup.js';
import {
  BUILTINS, BUILTIN_NAMES, DEFAULT_SETTINGS, DisplayClient, MAX_SOURCE_PIXELS, MOCK_PANELS, MockDisplayDevice, builtinPicture, compose, comparePlan,
  deepest, depthChoices, describeSettings, deviceLine, displayAnswer, displayHello, levelsToRgba, mockPort, partialLine, preparePicture, preparedPng,
  refreshChoices, refreshOptions, reportText,
} from './picture.js';

const HELLO_MS = 3000;
// a full refresh of the largest panel takes several seconds; the device answers when the panel is done
const SHOW_MS = 120000;
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

/** A picture file or clipboard image as RGBA pixels, by the browser's own decoder (EXIF turn applied). */
export async function decodeInBrowser(blob) {
  const bmp = await createImageBitmap(blob);
  try {
    if (bmp.width * bmp.height > MAX_SOURCE_PIXELS) throw new Error('too large');
    const c = document.createElement('canvas');
    c.width = bmp.width; c.height = bmp.height;
    const ctx = c.getContext('2d');
    ctx.drawImage(bmp, 0, 0);
    return { w: bmp.width, h: bmp.height, rgba: ctx.getImageData(0, 0, bmp.width, bmp.height).data };
  } finally { bmp.close?.(); }
}

/**
 * Start the page in `env` ({ document, navigator, location, ImageData, decodeImage, sleep? }): returns { ready, connect, disconnect, state, load }.
 * `ready` resolves when the preview device, if asked for, is attached.
 */
export function startPicturePage(env) {
  const doc = env.document;
  const $ = (id) => {
    const el = doc.getElementById(id);
    if (!el) throw new Error(`picture.html has no #${id}`);
    return el;
  };
  const sleep = env.sleep ?? wait;
  const serial = env.navigator && 'serial' in env.navigator ? env.navigator.serial : null;
  const state = { link: null, port: null, hello: null, client: null, preview: null, src: null, name: '', composed: null, composedKey: '', prepared: null,
    choices: [], sends: [], busy: false, cancel: false, zoom: false };
  const canvas = $('canvas');
  const message = (e) => (e && e.message ? e.message : String(e));
  const device = (text, cls) => { $('device').textContent = text; $('device').className = cls || 'mut'; };
  const say = (text, bad) => { $('status').textContent = text; $('status').className = bad ? 'warn' : ''; $('status').dataset.state = bad ? 'error' : 'ok'; };
  const pictureText = () => (state.src ? `${state.name}, ${state.src.w} x ${state.src.h} pixels` : '');
  const report = () => {
    $('report').value = reportText({ hello: state.hello, picture: pictureText(), sends: state.sends, saw: $('saw').value });
  };
  const option = (value, text) => { const o = doc.createElement('option'); o.value = value; o.textContent = text; return o; };

  const depthChoice = () => state.choices.find((c) => c.key === $('depth').value) ?? state.choices.at(-1);
  const settings = () => {
    const c = depthChoice();
    const rot = $('rotate').value;
    return { ...DEFAULT_SETTINGS, fit: $('fit').value, border: $('border').value, rotate: rot === 'auto' ? 'auto' : Number(rot),
      depth: c.depth, color: c.color, dither: $('dither').value, threshold: Number($('threshold').value) };
  };
  const refresh = () => $('refresh').value;

  /** The refresh choices for the depth now: unavailable ones disabled with the reason; a chosen one that went away falls back to auto. */
  function refreshOptionsFor() {
    const choices = refreshChoices(state.hello, depthChoice());
    const keep = $('refresh').value;
    $('refresh').replaceChildren(...choices.map((c) => { const o = option(c.key, c.enabled ? c.label : `${c.label} (not available)`); o.disabled = !c.enabled; return o; }));
    const ok = choices.find((c) => c.key === keep && c.enabled);
    $('refresh').value = ok ? keep : 'auto';
    const off = choices.find((c) => !c.enabled);
    $('refreshwhy').textContent = off ? `Partial is not available: ${off.why}.` : '';
  }

  function paint(p) {
    if (canvas.width !== p.w) canvas.width = p.w;
    if (canvas.height !== p.h) canvas.height = p.h;
    const ctx = canvas.getContext('2d');
    if (ctx) ctx.putImageData(new env.ImageData(levelsToRgba(p.levels, p.depth, p.color), p.w, p.h), 0, 0);
    canvas.style.width = state.zoom ? `${p.w}px` : '';
    canvas.style.imageRendering = state.zoom ? 'pixelated' : 'auto'; // shrunk to the page: smoothed, as the eye would; 1:1: every pixel as it is
  }

  /** The picture prepared with the controls as they are; the scaled picture is kept while only depth or dithering change. */
  function rebuild() {
    const s = settings();
    $('thresholdrow').hidden = !(s.dither === 'none' && !s.color && s.depth === 1);
    $('thresholdvalue').textContent = String(s.threshold);
    refreshOptionsFor();
    const ready = !!state.src && !!state.hello;
    $('send').disabled = !ready || state.busy;
    $('compare').disabled = !ready || state.busy;
    if (!ready) { report(); return; }
    const panel = { w: state.hello.w, h: state.hello.h };
    const key = [s.fit, s.border, s.rotate, s.color].join();
    const t0 = Date.now();
    try {
      if (state.composedKey !== key || !state.composed) { state.composed = compose(state.src, panel, s); state.composedKey = key; }
      state.prepared = preparePicture(state.src, panel, s, state.composed);
    } catch (e) { $('prepnote').textContent = `This picture cannot be prepared: ${message(e)}.`; $('send').disabled = true; $('compare').disabled = true; return; }
    paint(state.prepared);
    $('prepnote').textContent = `Prepared ${panel.w} x ${panel.h}, ${describeSettings(s, state.prepared.rotation, refresh()).replace(/, \w+ refresh$/, '')} (${Date.now() - t0} ms). These are the pixels that will be sent.`;
    report();
  }

  function setSource(src, name) {
    state.src = src; state.name = name; state.composed = null; state.composedKey = '';
    $('source').textContent = `Picture: ${name}, ${src.w} x ${src.h} pixels.`;
    $('source').className = 'mut';
    rebuild();
  }
  async function load(blob, name) {
    if (!state.hello) { $('source').textContent = 'Connect a device first: the picture is prepared for its panel.'; $('source').className = 'warn'; return; }
    try { setSource(await env.decodeImage(blob), name); } catch (e) {
      $('source').textContent = `This file could not be opened as a picture${e && e.message === 'too large' ? ' (too large)' : ''}. Try a PNG or JPEG.`;
      $('source').className = 'warn';
    }
  }
  function builtin(kind) {
    if (!state.hello) return;
    setSource(builtinPicture(kind, state.hello.w, state.hello.h, 2 ** Math.max(...state.hello.depths)), BUILTIN_NAMES[kind]);
  }

  function lock(on) {
    state.busy = on;
    for (const id of ['send', 'clear', 'compare', 'connect']) $(id).disabled = on || (id === 'connect' && !serial);
    $('stop').disabled = !on || !state.compare;
    if (!on) { state.compare = false; rebuild(); $('clear').disabled = !state.client; }
  }

  /** One send; the status says what was asked, what the device did and why, the panel time, the total time. */
  async function sendOne(prepared, ref, label) {
    const what = `${label ? `${label}: ` : ''}${describeSettings(prepared.settings, prepared.rotation, ref)}`;
    say(`Asked for ${what}; waiting for the panel...`);
    const t0 = Date.now(), mark = state.link ? state.link.logCount : 0;
    const logged = () => (state.link ? state.link.log.filter((l) => l.n > mark).map((l) => l.text) : []);
    let record;
    try {
      const png = await preparedPng(prepared);
      const r = await state.client.show(png, state.hello, refreshOptions(ref));
      const total = Date.now() - t0;
      const answer = `${displayAnswer(r)} Area ${r.w} x ${r.h} at ${r.x}, ${r.y}.`;
      record = { label, settings: describeSettings(prepared.settings, prepared.rotation, ref), ok: true, answer, totalMs: total, lines: logged() };
      say(`Asked for ${what}. ${answer} Panel ${r.ms} ms, ${total} ms in all.`);
    } catch (e) {
      const total = Date.now() - t0;
      record = { label, settings: describeSettings(prepared.settings, prepared.rotation, ref), ok: false, answer: `${message(e)}.`, totalMs: total, lines: logged() };
      say(`Asked for ${what}. Not shown: ${message(e)}. ${total} ms.`, true);
    }
    state.sends.push(record);
    if (state.sends.length > 12) state.sends.shift();
    report();
    return record;
  }

  async function send() {
    if (state.busy || !state.prepared || !state.client) return;
    lock(true);
    try { await sendOne(state.prepared, refresh(), ''); } finally { lock(false); }
  }
  async function clear() {
    if (state.busy || !state.client) return;
    lock(true);
    const t0 = Date.now(), mark = state.link ? state.link.logCount : 0;
    const logged = () => (state.link ? state.link.log.filter((l) => l.n > mark).map((l) => l.text) : []);
    say('Asked for a full refresh to white; waiting for the panel...');
    try {
      const r = await state.client.clear();
      const total = Date.now() - t0, answer = displayAnswer(r);
      state.sends.push({ label: 'Clear', settings: 'full refresh to white', ok: true, answer, totalMs: total, lines: logged() });
      say(`Asked for a full refresh to white. ${answer} Panel ${r.ms} ms, ${total} ms in all.`);
    } catch (e) {
      state.sends.push({ label: 'Clear', settings: 'full refresh to white', ok: false, answer: `${message(e)}.`, totalMs: Date.now() - t0, lines: logged() });
      say(`Asked for a full refresh to white. Not shown: ${message(e)}.`, true);
    } finally { lock(false); }
    report();
  }

  /** The same picture four ways, each a full refresh, with the pause between; the line says which is on the panel. */
  async function compare() {
    if (state.busy || !state.src || !state.client) return;
    const pause = Math.max(0, Math.min(120, Number($('pause').value) || 0));
    const base = settings(), steps = comparePlan(state.hello), panel = { w: state.hello.w, h: state.hello.h };
    state.compare = true;
    lock(true);
    $('stop').disabled = false;
    state.cancel = false;
    try {
      for (let i = 0; i < steps.length && !state.cancel; i++) {
        const st = steps[i];
        const s = { ...base, depth: st.choice.depth, color: st.choice.color, dither: st.dither };
        const prepared = preparePicture(state.src, panel, s, s.color === base.color && state.composed ? state.composed : compose(state.src, panel, s));
        paint(prepared);
        $('comparestatus').textContent = `Compare ${i + 1} of ${steps.length}: sending ${st.label}...`;
        const rec = await sendOne(prepared, 'full', `Compare ${i + 1} of ${steps.length}, ${st.label}`);
        if (!rec.ok) { $('comparestatus').textContent = `Compare stopped at ${i + 1} of ${steps.length}: ${st.label} was not shown.`; return; }
        $('comparestatus').textContent = `Compare ${i + 1} of ${steps.length}: ${st.label} is on the panel now.${i + 1 < steps.length ? ` Next one in ${pause} s.` : ' That was the last one.'}`;
        for (let left = pause * 1000; left > 0 && !state.cancel && i + 1 < steps.length; left -= 250) await sleep(Math.min(250, left));
      }
      if (state.cancel) $('comparestatus').textContent = 'Compare stopped.';
    } catch (e) { $('comparestatus').textContent = `Compare stopped: ${message(e)}.`; } finally { lock(false); }
  }

  /** The tools for this device's panel; the picture chosen stays while the page is open. */
  function show(hello, client) {
    state.hello = hello; state.client = client;
    state.choices = depthChoices(hello);
    $('depth').replaceChildren(...state.choices.map((c) => option(c.key, c.label)));
    $('depth').value = deepest(hello).key;
    $('pad').hidden = false;
    $('clear').disabled = false;
    state.composed = null; state.composedKey = '';
    rebuild();
  }

  async function disconnect(text) {
    const link = state.link;
    state.link = null; state.port = null; state.preview = null; state.client = null;
    state.cancel = true;
    $('send').disabled = true; $('clear').disabled = true; $('compare').disabled = true;
    $('connect').textContent = 'Connect';
    $('preview').hidden = true;
    $('partial').hidden = true;
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
    device(deviceLine(hello), 'ok');
    $('partial').textContent = partialLine(hello);
    $('partial').hidden = false;
    $('connect').textContent = preview ? 'Connect a device' : 'Disconnect';
    $('preview').textContent = preview ? `Preview: no device is attached. A pretend ${MOCK_PANELS[preview]} answers, so nothing reaches a panel.` : '';
    $('preview').hidden = !preview;
    show(hello, new DisplayClient({ requestRaw: (cmd, req) => link.requestRaw(cmd, req, { timeoutMs: SHOW_MS }) }));
  }

  async function connect() {
    if (state.link && !state.preview) { await disconnect('Disconnected. Connect again to send.'); return; }
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
    } catch (e) { device(message(e), 'warn'); }
    $('connect').disabled = false;
  }

  for (const id of ['fit', 'border', 'rotate', 'depth', 'dither']) $(id).onchange = rebuild;
  $('threshold').oninput = rebuild;
  $('refresh').onchange = rebuild;
  $('send').onclick = send;
  $('clear').onclick = clear;
  $('compare').onclick = compare;
  $('stop').onclick = () => { state.cancel = true; };
  $('saw').oninput = report;
  for (const kind of BUILTINS) $(`b-${kind}`).onclick = () => builtin(kind);
  $('file').onchange = () => { const f = $('file').files && $('file').files[0]; if (f) void load(f, f.name); };
  $('zoom').onclick = () => {
    state.zoom = !state.zoom;
    $('zoom').setAttribute('aria-pressed', String(state.zoom));
    $('zoom').textContent = state.zoom ? 'Fit to width' : 'Show 1:1';
    $('zoomnote').textContent = state.zoom ? 'Shown 1:1: one panel pixel is one screen pixel; scroll to see the rest.' : 'Shown to fit the page width.';
    canvas.style.width = state.zoom && state.prepared ? `${state.prepared.w}px` : '';
    canvas.style.imageRendering = state.zoom ? 'pixelated' : 'auto';
  };
  const drop = $('drop');
  if (drop.addEventListener) {
    drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.className = 'over'; });
    drop.addEventListener('dragleave', () => { drop.className = ''; });
    drop.addEventListener('drop', (e) => {
      e.preventDefault(); drop.className = '';
      const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
      if (f) void load(f, f.name);
    });
  }
  const fromItems = (items) => {
    for (const it of items ?? []) if (it.type && it.type.startsWith('image/')) { const f = it.getAsFile ? it.getAsFile() : it; if (f) return f; }
    return null;
  };
  if (doc.addEventListener) doc.addEventListener('paste', (e) => {
    const f = fromItems(e.clipboardData && e.clipboardData.items);
    if (f) { e.preventDefault(); void load(f, 'pasted picture'); }
  });
  $('pastebtn').onclick = async () => {
    try {
      for (const item of await env.navigator.clipboard.read()) {
        const type = item.types.find((t) => t.startsWith('image/'));
        if (type) { await load(await item.getType(type), 'pasted picture'); return; }
      }
      $('source').textContent = 'There is no picture on the clipboard.'; $('source').className = 'warn';
    } catch { $('source').textContent = 'The browser did not give this page the clipboard. Press Ctrl+V (or Cmd+V) on the page instead.'; $('source').className = 'warn'; }
  };
  $('reportcopy').onclick = async () => {
    const text = $('report').value;
    try { await env.navigator.clipboard.writeText(text); $('reportnote').textContent = 'Copied.'; } catch {
      try { $('report').select?.(); $('reportnote').textContent = doc.execCommand?.('copy') ? 'Copied.' : 'Select the text above and copy it.'; } catch { $('reportnote').textContent = 'Select the text above and copy it.'; }
    }
  };
  $('connect').onclick = connect;
  $('connect').disabled = !serial;
  $('nosupport').hidden = !!serial;
  report();
  if (serial && serial.addEventListener) {
    serial.addEventListener('disconnect', (e) => {
      if (state.port && e.target === state.port) void disconnect('The device was unplugged. Plug it in and connect again.');
    });
  }

  const mock = new URLSearchParams(env.location.search).get('mock');
  let ready = Promise.resolve();
  if (mock !== null && !Object.hasOwn(MOCK_PANELS, mock)) {
    $('preview').textContent = `There is no preview panel "${mock}": use ?mock=x, ?mock=og or ?mock=picpak.`;
    $('preview').hidden = false;
  } else if (mock !== null) {
    ready = attach(mockPort(new MockDisplayDevice(mock)), mock).catch((e) => device(message(e), 'warn'));
  }
  return { ready, connect, disconnect, state, load };
}

if (typeof document !== 'undefined') {
  startPicturePage({ document, navigator, location, ImageData, decodeImage: decodeInBrowser });
}
