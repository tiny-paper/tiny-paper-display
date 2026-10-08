// SPDX-License-Identifier: MIT
// The pages that are written in Markdown: `npm run pages` (in site/) writes them from docs/, and
// site/test/generated.test.mjs fails when a committed page is not what its source makes.
//   docs/use.md                -> api.html       the guide ("Use it", in the navigation)
//   docs/spec/usb-display.md   -> protocol.html  the byte-level reference (linked from the guide, not from the navigation)
// marked is pinned in package.json. Links in the Markdown are written relative to the file (../site/sketch.html), so they
// work on GitHub too; here they become page-relative (sketch.html).
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Marked } from 'marked';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

export const NAV = [['index.html', 'Flash'], ['sketch.html', 'Sketch'], ['picture.html', 'Picture'], ['api.html', 'Use it']];
export const PAGES = [
  { src: 'docs/use.md', out: 'api.html', title: 'Use it', current: 'api.html' },
  { src: 'docs/spec/usb-display.md', out: 'protocol.html', title: 'USB display protocol', current: null },
];

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const slug = (s) => s.toLowerCase().replace(/<[^>]+>/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

const marked = new Marked({
  gfm: true,
  renderer: {
    // "```python test": the first word is the language, the rest only marks a sample that a test runs
    code({ text, lang }) {
      const l = (lang || '').split(/\s+/)[0];
      return `<pre><code${l ? ` class="language-${esc(l)}"` : ''}>${esc(text)}\n</code></pre>\n`;
    },
    heading({ tokens, depth }) {
      const inner = this.parser.parseInline(tokens);
      return `<h${depth} id="${slug(inner)}">${inner}</h${depth}>\n`;
    },
    link({ href, tokens }) {
      const to = href.replace(/^\.\.\/site\//, '');
      return `<a href="${esc(to)}">${this.parser.parseInline(tokens)}</a>`;
    },
  },
});

const STYLE = 'body{font:16px/1.5 system-ui,sans-serif;max-width:60rem;margin:0 auto;padding:1rem;color:#111;background:#fff}'
  + 'table{border-collapse:collapse;margin:1rem 0;display:block;overflow-x:auto}td,th{border:1px solid #bbb;padding:.3rem .6rem;text-align:left;vertical-align:top}'
  + 'nav a{margin-right:1rem}nav a[aria-current]{font-weight:600;text-decoration:none}'
  + 'code,pre{font:13px/1.4 ui-monospace,monospace;background:#f2f2f2}code{padding:0 .2rem}pre{padding:.6rem;overflow-x:auto;max-width:100%}pre code{padding:0}'
  + 'h2{margin-top:2rem}h3{margin-top:1.5rem}'
  + '@media(prefers-color-scheme:dark){body{color:#eee;background:#141414}code,pre{background:#2a2a2a}td,th{border-color:#555}a{color:#8ab4f8}}';

/** One page as HTML text. */
export function render({ src, title, current }, md = readFileSync(join(root, src), 'utf8')) {
  const nav = NAV.map(([href, name]) => `<a href="${href}"${href === current ? ' aria-current="page"' : ''}>${name}</a>`).join('');
  return `<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n`
    + `<title>${esc(title)}</title>\n<style>${STYLE}</style>\n</head>\n<body>\n<nav>${nav}</nav>\n${marked.parse(md)}</body>\n</html>\n`;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  for (const p of PAGES) writeFileSync(join(here, p.out), render(p));
}
