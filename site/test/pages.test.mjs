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
const PAGES = ['index.html', 'sketch.html', 'picture.html', 'api.html'];
const NAV = [['index.html', 'Flash'], ['sketch.html', 'Sketch'], ['picture.html', 'Picture'], ['api.html', 'Use it']];

test('navigation: Flash, Sketch, Picture, Use it on every page, the page itself marked', () => {
  for (const page of PAGES) {
    const nav = read(page).match(/<nav>(.*?)<\/nav>/s);
    assert.ok(nav, `${page}: no <nav>`);
    const links = [...nav[1].matchAll(/<a href="([^"]+)"( aria-current="page")?>([^<]+)<\/a>/g)].map((m) => [m[1], m[3], !!m[2]]);
    assert.deepEqual(links, NAV.map(([href, name]) => [href, name, href === page]), page);
  }
});

test('the protocol page: same navigation, none marked, linked from the guide and not from the navigation', () => {
  const nav = read('protocol.html').match(/<nav>(.*?)<\/nav>/s)[1];
  const links = [...nav.matchAll(/<a href="([^"]+)"( aria-current="page")?>([^<]+)<\/a>/g)].map((m) => [m[1], m[3], !!m[2]]);
  assert.deepEqual(links, NAV.map(([href, name]) => [href, name, false]));
  assert.match(read('api.html'), /<a href="protocol\.html">/);
  for (const page of PAGES) assert.doesNotMatch(read(page).match(/<nav>(.*?)<\/nav>/s)[1], /protocol/, page);
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
  for (const f of ['sketch.js', 'sketch-page.js', 'picture.js', 'picture-page.js', 'bt-setup.js', 'frame.js', 'flash.js']) assert.ok(seen.has(f), `${f} is not reached from a page`);
  // pages.yml publishes with publish.mjs, which copies every .html and .js file of site/
  const workflow = readFileSync(join(SITE, '..', '.github/workflows/pages.yml'), 'utf8');
  assert.match(workflow, /node site\/publish\.mjs/);
  assert.match(readFileSync(join(SITE, 'publish.mjs'), 'utf8'), /!\/\\\.\(html\|js\)\$\/\.test\(f\)/, 'publish.mjs: copies .html and .js');
  assert.deepEqual([...seen].filter((f) => !/\.(html|js)$/.test(f)).sort(), [], 'loaded by a page but not copied by publish.mjs');
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

// a plain link to TRMNL's own instructions, nothing is loaded from it
const TRMNL_FLASHING_MODE = 'https://help.trmnl.com/en/articles/11936721-put-your-trmnl-in-flashing-mode';

test('the whole site loads one thing from elsewhere: esptool-js from jsDelivr, on the flash page', () => {
  const urls = new Set();
  for (const f of readdirSync(SITE).filter((n) => /\.(html|js)$/.test(n))) {
    // the Markdown pages load nothing (checked in generated.test.mjs); the addresses in their text are examples to type
    const text = ['api.html', 'protocol.html'].includes(f) ? read(f).replace(/<pre>[^]*?<\/pre>/g, '') : read(f);
    for (const m of text.matchAll(/https?:\/\/[^\s"'<>)]+/g)) urls.add(m[0]);
  }
  assert.deepEqual([...urls].sort(), ['https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js', 'https://github.com/espressif/esptool-js', TRMNL_FLASHING_MODE]);
});

test('the bundle names the tool that made it', () => {
  assert.match(read('sketch.js').split('\n')[0], /^\/\/ esbuild \d+\.\d+\.\d+, \d+ source files of site\/src \(MIT\): do not edit, run `npm run build` in site\/\.$/);
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
