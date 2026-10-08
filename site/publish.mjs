// SPDX-License-Identifier: MIT
// What the Pages workflow does to the site, as code a test can run (site/test/publish.test.mjs):
//   node site/publish.mjs <site dir> <out dir> main|testing <all releases json> [required tag]
// copies the pages and scripts to <out dir>, the testing copy with a visible "testing" label on every page, writes
// <out dir>/fw/releases.json (main: releases only; testing: pre-releases and releases; newest first) and prints the
// tags whose files must be copied next to it, one per line. A required tag that is not in the list is an error: the
// deploy after a release must not publish the old list.
import { copyFileSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const LIST_LIMIT = 5;
const LABEL = '<div id="testing-label" role="note" style="position:sticky;top:0;z-index:99;background:#b00020;color:#fff;'
  + 'font:700 14px/1.6 system-ui,sans-serif;text-align:center;letter-spacing:.1em;text-transform:uppercase">testing</div>';

/** A page with the label right after <body> and "testing" in front of its title. */
export function labelPage(html) {
  if (!/<body[^>]*>/.test(html)) throw new Error('a page without <body>');
  return html.replace(/<body[^>]*>/, (m) => `${m}\n${LABEL}`).replace(/<title>/, '<title>testing: ');
}

/** The releases a page lists: not drafts, no pre-releases on the main page, newest first, at most LIST_LIMIT. */
export function releaseList(all, testing) {
  return all
    .filter((r) => !r.isDraft && (testing || !r.isPrerelease))
    .sort((a, b) => String(b.publishedAt).localeCompare(String(a.publishedAt)))
    .slice(0, LIST_LIMIT)
    .map((r) => ({ tagName: r.tagName, publishedAt: r.publishedAt, isPrerelease: !!r.isPrerelease }));
}

/** Copy the site to `out`; returns the tags of its list. `tag`: must be in that list (else throws). */
export function assemble({ site, out, testing, all, tag }) {
  const list = releaseList(all, testing);
  if (tag && !list.some((r) => r.tagName === tag)) {
    throw new Error(`release ${tag} is not in the list of the ${testing ? 'testing' : 'main'} page (${list.map((r) => r.tagName).join(', ') || 'empty'}): not published`);
  }
  mkdirSync(join(out, 'fw'), { recursive: true });
  for (const f of readdirSync(site)) {
    if (!/\.(html|js)$/.test(f)) continue;
    if (testing && f.endsWith('.html')) writeFileSync(join(out, f), labelPage(readFileSync(join(site, f), 'utf8')));
    else copyFileSync(join(site, f), join(out, f));
  }
  writeFileSync(join(out, 'fw', 'releases.json'), `${JSON.stringify(list, null, 1)}\n`);
  return list.map((r) => r.tagName);
}

/**
 * After a deploy: the LIVE site lists `tag` and serves its files. Read with a query that no cache has seen (a page
 * is cached for ten minutes: an ordinary read can show the old list after a good deploy), and tried again for a while,
 * because a deployment takes a moment to go live. Throws when it never does: the job fails instead of reporting success.
 * `base`: the site's root URL with a trailing slash. A v* tag is checked on the main page and the testing page, a t* tag
 * on the testing page.
 */
export async function verifyLive({ base, tag = '', run = '', fetchFn = fetch, tries = 12, waitMs = 10000, sleep = (ms) => new Promise((r) => setTimeout(r, ms)) }) {
  const pages = tag.startsWith('v') ? [base, `${base}testing/`] : [`${base}testing/`];
  if (!tag) pages.splice(0, pages.length, base, `${base}testing/`);
  let last = '';
  for (let i = 1; i <= tries; i++) {
    try {
      last = '';
      for (const page of pages) {
        const bust = `cb=${Date.now()}${i}`;
        if (run) { // is this deployment the one that is live? (a deployment can report success and not be applied)
          const b = await fetchFn(`${page}build.json?${bust}`, { cache: 'no-store' });
          const live = b.ok ? (await b.json()).run : `HTTP ${b.status}`;
          if (String(live) !== String(run)) { last = `${page}build.json is run ${live}, this deploy is run ${run}: the deployment was not applied`; break; }
        }
        if (!tag) continue;
        const list = await fetchFn(`${page}fw/releases.json?${bust}`, { cache: 'no-store' });
        if (!list.ok) { last = `${page}fw/releases.json: HTTP ${list.status}`; break; }
        if (!(await list.json()).some((r) => r.tagName === tag)) { last = `${page}fw/releases.json does not list ${tag}`; break; }
        const file = await fetchFn(`${page}fw/${encodeURIComponent(tag)}/trmnl_x__flash.txt?${bust}`, { cache: 'no-store' });
        if (!file.ok) { last = `${page}fw/${tag}/trmnl_x__flash.txt: HTTP ${file.status}`; break; }
      }
    } catch (e) { last = String(e.message ?? e); }
    if (!last) return;
    if (i < tries) await sleep(waitMs);
  }
  throw new Error(`the live site does not show ${tag} after ${tries} tries: ${last}`);
}

if (process.argv[1] === fileURLToPath(import.meta.url) && process.argv[2] === 'verify') {
  const [, , , base, tag, run, tries] = process.argv;
  try {
    await verifyLive({ base: base.endsWith('/') ? base : `${base}/`, tag: tag || '', run: run || '', ...(tries ? { tries: Number(tries) } : {}) });
    console.log(`the live site is ${run ? `run ${run}` : 'up'}${tag ? ` and lists ${tag} and serves its files` : ''}`);
  } catch (e) {
    console.error(`publish: ${e.message}`);
    process.exit(1);
  }
} else if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [site, out, which, allFile, tag] = process.argv.slice(2);
  if (!site || !out || !['main', 'testing'].includes(which) || !allFile) {
    console.error('usage: publish.mjs <site dir> <out dir> main|testing <all releases json> [required tag]');
    process.exit(2);
  }
  try {
    const tags = assemble({ site, out, testing: which === 'testing', all: JSON.parse(readFileSync(allFile, 'utf8')), tag: tag || '' });
    process.stdout.write(tags.map((t) => `${t}\n`).join(''));
  } catch (e) {
    console.error(`publish: ${e.message}`);
    process.exit(1);
  }
}
