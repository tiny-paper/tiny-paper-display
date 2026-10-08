// SPDX-License-Identifier: MIT
// What the Pages workflow publishes (site/publish.mjs): the "testing" label only on the testing copy, each page's release
// list, a release the deploy was called for that is not listed stops it, and the triggers of the workflows.
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readFileSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { assemble, labelPage, releaseList } from '../publish.mjs';

const SITE = join(dirname(fileURLToPath(import.meta.url)), '..');
const WORKFLOWS = join(SITE, '..', '.github', 'workflows');
const PAGES = readdirSync(SITE).filter((f) => f.endsWith('.html'));

const ALL = [
  { tagName: 'v0.1.0', publishedAt: '2026-10-01T10:00:00Z', isPrerelease: false, isDraft: false },
  { tagName: 't20261008.1', publishedAt: '2026-10-08T10:00:00Z', isPrerelease: true, isDraft: false },
  { tagName: 'v0.2.0', publishedAt: '2026-10-05T10:00:00Z', isPrerelease: false, isDraft: false },
  { tagName: 't20261009.1', publishedAt: '2026-10-09T10:00:00Z', isPrerelease: true, isDraft: true },
];
const tmp = () => mkdtempSync(join(tmpdir(), 'pages-'));

test('the release lists: the main page only releases, the testing page pre-releases too, newest first, no drafts', () => {
  assert.deepEqual(releaseList(ALL, false).map((r) => r.tagName), ['v0.2.0', 'v0.1.0']);
  assert.deepEqual(releaseList(ALL, true).map((r) => r.tagName), ['t20261008.1', 'v0.2.0', 'v0.1.0']);
  assert.equal(releaseList(ALL, true)[0].isPrerelease, true);
  const many = Array.from({ length: 9 }, (_, i) => ({ tagName: `v1.0.${i}`, publishedAt: `2026-11-0${i + 1}T00:00:00Z`, isPrerelease: false, isDraft: false }));
  assert.deepEqual(releaseList(many, false).map((r) => r.tagName), ['v1.0.8', 'v1.0.7', 'v1.0.6', 'v1.0.5', 'v1.0.4']);
});

test('the testing label is on every page of the testing copy and on no page of the main copy; the lists are written next to each', () => {
  const main = tmp(), testing = tmp();
  assert.deepEqual(assemble({ site: SITE, out: main, testing: false, all: ALL }), ['v0.2.0', 'v0.1.0']);
  assert.deepEqual(assemble({ site: SITE, out: testing, testing: true, all: ALL }), ['t20261008.1', 'v0.2.0', 'v0.1.0']);
  assert.ok(PAGES.length >= 3);
  for (const page of PAGES) {
    assert.ok(!readFileSync(join(main, page), 'utf8').includes('testing-label'), `${page}: no label on the main page`);
    const t = readFileSync(join(testing, page), 'utf8');
    assert.match(t, /<body[^>]*>\s*<div id="testing-label"[^>]*>testing<\/div>/, `${page}: the label is the first thing on the page`);
    assert.match(t, /<title>testing: /);
    assert.ok(existsSync(join(testing, 'sketch.js')) && existsSync(join(main, 'sketch.js')));
  }
  const listed = (dir) => JSON.parse(readFileSync(join(dir, 'fw', 'releases.json'), 'utf8')).map((r) => r.tagName);
  assert.deepEqual(listed(main), ['v0.2.0', 'v0.1.0']);
  assert.deepEqual(listed(testing), ['t20261008.1', 'v0.2.0', 'v0.1.0']);
  assert.throws(() => labelPage('<p>no body</p>'), /without <body>/);
});

test('a deploy called for a release that is not in the list stops with an error: it never publishes the old list', () => {
  const out = tmp();
  assert.deepEqual(assemble({ site: SITE, out, testing: false, all: ALL, tag: 'v0.2.0' }), ['v0.2.0', 'v0.1.0']);
  assert.throws(() => assemble({ site: SITE, out: tmp(), testing: false, all: ALL, tag: 'v0.3.0' }), /v0\.3\.0 is not in the list of the main page/);
  assert.throws(() => assemble({ site: SITE, out: tmp(), testing: false, all: ALL, tag: 't20261008.1' }), /not in the list of the main page/, 'a test build is not on the main page');
  assert.throws(() => assemble({ site: SITE, out: tmp(), testing: true, all: ALL, tag: 't20261009.1' }), /not in the list of the testing page/, 'a draft is not listed');
});

test('workflows: pages for both branches with the tag handed over, t* tags build a pre-release, publish.yml on v* only', () => {
  const pages = readFileSync(join(WORKFLOWS, 'pages.yml'), 'utf8');
  assert.match(pages, /push:\s*\n\s*branches: \[main, testing\]/);
  assert.match(pages, /workflow_call:\s*\n\s*inputs:\s*\n\s*tag:/);
  assert.match(pages, /ref: main, path: main-src/);
  assert.match(pages, /ref: testing, path: testing-src/);
  assert.match(pages, /publish testing-src\/site _site\/testing testing/);
  const build = readFileSync(join(WORKFLOWS, 'build.yml'), 'utf8');
  assert.match(build, /tags: \['v\*', 't\*'\]/);
  assert.match(build, /kind="--prerelease"/);
  assert.match(build, /tag: \$\{\{ github\.ref_name \}\}/);
  const publish = readFileSync(join(WORKFLOWS, 'publish.yml'), 'utf8');
  assert.match(publish, /tags: \['v\*'\]/);
  assert.ok(!/'t\*'|branches/.test(publish.split('jobs:')[0]), 'publish.yml must not run for t* tags or branches');
});
