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

if (process.argv[1] === fileURLToPath(import.meta.url)) {
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
