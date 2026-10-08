// SPDX-License-Identifier: MIT
// What the Pages workflow publishes (site/publish.mjs): the "testing" label only on the testing copy, each page's release
// list, a release the deploy was called for that is not listed stops it, and the triggers of the workflows.
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readFileSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { assemble, labelPage, releaseList, verifyLive } from '../publish.mjs';

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

const json = (body, ok = true, status = 200) => ({ ok, status, json: async () => body });
const live = (state) => async (url) => {
  state.calls.push(url);
  if (url.includes('releases.json')) return json(state.list(url));
  return json({}, state.file, state.file ? 200 : 404);
};

test('verifyLive: the live lists are read past every cache, retried until they show the release, and fail loudly when they never do', async () => {
  const base = 'https://example.test/site/';
  const slept = [];
  const sleep = async (ms) => { slept.push(ms); };
  // the old list for two reads, then the new one: a deployment going live
  const state = { calls: [], file: true, list: () => (state.calls.filter((u) => u.includes('releases.json')).length < 3 ? [{ tagName: 'v0.1.0' }] : [{ tagName: 't1' }, { tagName: 'v0.1.0' }]) };
  await verifyLive({ base, tag: 't1', fetchFn: live(state), sleep, waitMs: 1 });
  assert.equal(slept.length, 2, 'two waits before the list showed the tag');
  assert.ok(state.calls.every((u) => /[?&]cb=\d+/.test(u)), 'every read has a query no cache has seen');
  assert.ok(state.calls.some((u) => u.startsWith(`${base}testing/fw/t1/trmnl_x__flash.txt`)), 'its files are read too');
  assert.ok(!state.calls.some((u) => u.startsWith(`${base}fw/releases.json`)), 'a t* tag is not on the main page');
  // a v* tag is on both pages
  const both = { calls: [], file: true, list: () => [{ tagName: 'v1.0.0' }] };
  await verifyLive({ base, tag: 'v1.0.0', fetchFn: live(both), sleep });
  assert.ok(both.calls.some((u) => u.startsWith(`${base}fw/releases.json`)) && both.calls.some((u) => u.startsWith(`${base}testing/fw/releases.json`)));
  // never listed
  await assert.rejects(verifyLive({ base, tag: 't2', fetchFn: live({ calls: [], file: true, list: () => [{ tagName: 't1' }] }), sleep, tries: 3 }), /does not show t2 after 3 tries: .*does not list t2/);
  // listed but the files are missing
  await assert.rejects(verifyLive({ base, tag: 't1', fetchFn: live({ calls: [], file: false, list: () => [{ tagName: 't1' }] }), sleep, tries: 2 }), /trmnl_x__flash\.txt: HTTP 404/);
  // the site unreachable
  await assert.rejects(verifyLive({ base, tag: 't1', fetchFn: async () => { throw new Error('offline'); }, sleep, tries: 2 }), /offline/);
});

test('the flash page reads its release list past the cache; pages.yml checks the live site after a release deploy', () => {
  assert.match(readFileSync(join(SITE, 'index.html'), 'utf8'), /fetch\(`fw\/releases\.json\?cb=\$\{Date\.now\(\)\}`, \{ cache: 'no-store' \}\)/);
  const pages = readFileSync(join(WORKFLOWS, 'pages.yml'), 'utf8');
  assert.match(pages, /run: node site\/publish\.mjs verify "\$PAGE_URL" "\$TAG" "\$GITHUB_RUN_ID"/, 'every deploy asks the live site whether it is this run');
  assert.match(pages, /tee _site\/build\.json > _site\/testing\/build\.json/);
});

test('verifyLive: a deployment that reports success but is not the live one is caught by its run id', async () => {
  const base = 'https://example.test/site/';
  const site = (run, tags) => async (url) => {
    if (url.includes('build.json')) return json({ run });
    if (url.includes('releases.json')) return json(tags.map((t) => ({ tagName: t })));
    return json({});
  };
  await verifyLive({ base, tag: 't9', run: '77', fetchFn: site('77', ['t9']), sleep: async () => {}, tries: 2 });
  await assert.rejects(verifyLive({ base, tag: 't9', run: '77', fetchFn: site('76', ['t9']), sleep: async () => {}, tries: 2 }), /build\.json is run 76, this deploy is run 77: the deployment was not applied/);
  await assert.rejects(verifyLive({ base, tag: 't9', run: '77', fetchFn: site('77', ['t8']), sleep: async () => {}, tries: 2 }), /does not list t9/);
  // no tag: only the run is checked, on both pages
  const seen = [];
  await verifyLive({ base, run: '5', fetchFn: async (u) => { seen.push(u.split('?')[0]); return json({ run: '5' }); }, sleep: async () => {} });
  assert.deepEqual(seen, [`${base}build.json`, `${base}testing/build.json`]);
});
