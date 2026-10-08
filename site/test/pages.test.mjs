// SPDX-License-Identifier: MIT
// The site as a whole: Flash, Sketch and API link to each other on every page, every file a page loads exists and is
// copied by the Pages workflow, the Sketch page loads only the site's own files, the flash page points to the Sketch
// page after an install. Run: node --test site/test/*.test.mjs
import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

const SITE = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (name) => readFileSync(join(SITE, name), 'utf8');
const PAGES = ['index.html', 'sketch.html', 'api.html'];
const NAV = [['index.html', 'Flash'], ['sketch.html', 'Sketch'], ['api.html', 'API']];

test('navigation: Flash, Sketch, API on every page, the page itself marked', () => {
  for (const page of PAGES) {
    const nav = read(page).match(/<nav>(.*?)<\/nav>/s);
    assert.ok(nav, `${page}: no <nav>`);
    const links = [...nav[1].matchAll(/<a href="([^"]+)"( aria-current="page")?>([^<]+)<\/a>/g)].map((m) => [m[1], m[3], !!m[2]]);
    assert.deepEqual(links, NAV.map(([href, name]) => [href, name, href === page]), page);
  }
});

/** The site's own files a page or script loads: script src, module imports, links to pages. */
function loads(name) {
  const text = read(name);
  const found = name.endsWith('.html')
    ? [...text.matchAll(/<script\b[^>]*\bsrc="([^"]+)"/g), ...text.matchAll(/<a\b[^>]*\bhref="([^"#?]+)[^"]*"/g), ...text.matchAll(/\bfrom '(\.\/[^']+)'/g)]
    : [...text.matchAll(/^import\b[^'"]*['"](\.\/[^'"]+)['"]/gm)];
  return found.map((m) => m[1]).filter((f) => !/^[a-z]+:/.test(f)).map((f) => f.replace(/^\.\//, ''));
}

test('every file a page loads is there and is copied to the published site', () => {
  const seen = new Set(PAGES);
  for (const queue = [...PAGES]; queue.length;) {
    const name = queue.pop();
    for (const f of loads(name)) {
      assert.ok(existsSync(join(SITE, f)), `${name} loads ${f}: not in site/`);
      if (!seen.has(f)) { seen.add(f); queue.push(f); }
    }
  }
  for (const f of ['sketch.js', 'sketch-page.js', 'bt-setup.js', 'frame.js', 'flash.js']) assert.ok(seen.has(f), `${f} is not reached from a page`);
  const workflow = readFileSync(join(SITE, '..', '.github/workflows/pages.yml'), 'utf8');
  const cp = workflow.match(/^\s*cp (.+?) _site\/(\s|$)/m);
  assert.ok(cp, 'pages.yml: no cp ... _site/');
  const all = readdirSync(SITE);
  const copied = new Set(cp[1].trim().split(/\s+/).flatMap((pattern) => {
    assert.ok(pattern.startsWith('site/'), pattern);
    const rx = new RegExp(`^${pattern.slice(5).replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '[^/]*')}$`);
    return all.filter((f) => rx.test(f));
  }));
  assert.deepEqual([...seen].filter((f) => !copied.has(f)).sort(), [], 'loaded by a page but not copied by pages.yml');
});

test('the Sketch page loads only the site\'s own files: its policy says so and nothing in it needs more', () => {
  const html = read('sketch.html');
  const csp = html.match(/<meta http-equiv="Content-Security-Policy" content="([^"]+)">/);
  assert.ok(csp, 'sketch.html: no Content-Security-Policy');
  const rules = Object.fromEntries(csp[1].split(';').map((r) => r.trim().split(/\s+/)).map(([k, ...v]) => [k, v.join(' ')]));
  assert.equal(rules['default-src'], "'none'");
  assert.equal(rules['script-src'], "'self'");
  assert.equal(rules['connect-src'], undefined, 'the page fetches nothing');
  const scripts = [...html.matchAll(/<script\b([^>]*)>([^]*?)<\/script>/g)];
  assert.equal(scripts.length, 1);
  assert.match(scripts[0][1], /\bsrc="sketch-page\.js"/);
  assert.equal(scripts[0][2].trim(), '', 'no inline script: the policy would stop it');
  assert.doesNotMatch(html, /\son\w+="/, 'no inline handlers');
  for (const f of ['sketch.html', 'sketch-page.js', 'sketch.js', 'bt-setup.js', 'frame.js']) {
    assert.doesNotMatch(read(f), /https?:\/\/|\bfetch\(|XMLHttpRequest|WebSocket|\bimport\(/, `${f} reaches outside the site`);
  }
});

test('the whole site loads one thing from elsewhere: esptool-js from jsDelivr, on the flash page', () => {
  const urls = new Set();
  for (const f of readdirSync(SITE).filter((n) => /\.(html|js)$/.test(n))) {
    for (const m of read(f).matchAll(/https?:\/\/[^\s"'<>)]+/g)) urls.add(m[0]);
  }
  assert.deepEqual([...urls].sort(), ['https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js', 'https://github.com/espressif/esptool-js']);
});

test('the bundle names the tool that made it', () => {
  assert.match(read('sketch.js').split('\n')[0], /^\/\/ esbuild \d+\.\d+\.\d+, \d+ source files of the tiny-paper main repository \(MIT\): do not edit\.$/);
});

test('after an install the flash page says "Installed: now draw on it" with the link, and not before', () => {
  const html = read('index.html');
  assert.match(html, /<p id="next" class="ok" hidden>Installed: now <a href="sketch\.html">draw on it<\/a>\.<\/p>/);
  const click = html.slice(html.indexOf("$('install').onclick"), html.indexOf('// Bluetooth setup over the cable'));
  const start = click.indexOf("$('next').hidden = true;");
  const done = click.indexOf('await install(');
  const shown = click.indexOf("$('next').hidden = false;");
  const failed = click.indexOf('} catch');
  assert.ok(start > 0 && start < done, 'hidden again when an install starts');
  assert.ok(done < shown && shown < failed, 'shown only after install() returned, inside the try');
});
