// SPDX-License-Identifier: MIT
// The Sketch page's modules as one browser file: `npm run build` (in site/) writes sketch.js from src/. Same input,
// same bytes: esbuild is pinned (package.json, exact) and named in the first line of the output; no minifying, so the
// published file can be read against its sources. site/test/bundle.test.mjs fails when sketch.js is not what src/ makes.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const here = dirname(fileURLToPath(import.meta.url));

/** The bundles: the file each page loads, and the entry it is made from. */
export const BUNDLES = { 'sketch.js': 'src/display/index.ts', 'picture.js': 'src/picture/index.ts' };

/** One bundle as text (the Sketch page's by default). Throws when the installed esbuild is not the pinned one. */
export async function bundle(out = 'sketch.js') {
  const pinned = JSON.parse(readFileSync(join(here, 'package.json'), 'utf8')).devDependencies.esbuild;
  if (esbuild.version !== pinned) throw new Error(`esbuild ${esbuild.version} is installed, package.json pins ${pinned}: run npm ci in site/`);
  const r = await esbuild.build({
    absWorkingDir: here,
    entryPoints: [BUNDLES[out]],
    bundle: true, format: 'esm', platform: 'browser', target: 'es2022', charset: 'ascii', legalComments: 'none',
    write: false, metafile: true, logLevel: 'error',
  });
  const inputs = Object.keys(r.metafile.inputs).length;
  return `// esbuild ${esbuild.version}, ${inputs} source files of site/src (MIT): do not edit, run \`npm run build\` in site/.\n${r.outputFiles[0].text}`;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) for (const out of Object.keys(BUNDLES)) writeFileSync(join(here, out), await bundle(out));
