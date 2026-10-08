// SPDX-License-Identifier: MIT
// The few parts of a document the Sketch page touches, made from the page's own HTML: an id the script asks for that
// sketch.html does not have is null here too. Not a test file (the runner takes *.test.mjs).

export class FakeElement {
  constructor(tag, id = '', hidden = false) {
    Object.assign(this, { tag, id, hidden, disabled: false, textContent: '', className: '', value: '', title: '', width: 0, height: 0,
      dataset: {}, style: {}, attrs: {}, children: [], listeners: {}, onclick: null, onchange: null, oninput: null, puts: [] });
  }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return this.attrs[k] ?? null; }
  replaceChildren(...c) { this.children = c; }
  addEventListener(type, f) { (this.listeners[type] ??= []).push(f); }
  /** Deliver an event to the listeners, as the browser would. */
  dispatch(type, e) { for (const f of this.listeners[type] ?? []) f(e); }
  getContext() { return { putImageData: (img, x, y) => { this.puts.push({ x, y, w: img.width, h: img.height }); } }; }
  getBoundingClientRect() { return { left: 0, top: 0, width: this.width, height: this.height }; }
  setPointerCapture() {}
}

export class FakeImageData {
  constructor(data, width, height) {
    if (data.length !== width * height * 4) throw new Error('ImageData: wrong size');
    Object.assign(this, { data, width, height });
  }
}

/** A document with one element per `id="..."` of the HTML (its tag, its `hidden`). */
export function fakeDocument(html) {
  const byId = new Map();
  for (const m of html.matchAll(/<(\w+)\b([^>]*?\bid="([^"]+)"[^>]*)>/g)) byId.set(m[3], new FakeElement(m[1], m[3], /\shidden\b/.test(m[2])));
  return { getElementById: (id) => byId.get(id) ?? null, createElement: (tag) => new FakeElement(tag) };
}

/** A pointer event on the canvas at panel pixel x, y (the fake canvas is shown 1:1). */
export const pointer = (x, y, id = 1) => ({ pointerId: id, pointerType: 'mouse', button: 0, clientX: x, clientY: y, preventDefault() {} });
