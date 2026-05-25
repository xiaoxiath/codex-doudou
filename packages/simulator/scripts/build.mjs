#!/usr/bin/env node
import { build, context } from 'esbuild';
import { mkdir, cp, rm } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = resolve(__dirname, '..');
const outdir = resolve(root, 'public');
const watch = process.argv.includes('--watch');

await rm(resolve(outdir, 'main.js'), { force: true });
await rm(resolve(outdir, 'main.js.map'), { force: true });
await mkdir(outdir, { recursive: true });

const options = {
  entryPoints: [resolve(root, 'src/main.ts')],
  outfile: resolve(outdir, 'main.js'),
  bundle: true,
  format: 'esm',
  target: 'es2022',
  sourcemap: true,
  logLevel: 'info',
  define: { 'process.env.NODE_ENV': '"development"' },
};

if (watch) {
  const ctx = await context(options);
  await ctx.watch();
  console.log('simulator: watching for changes…');
} else {
  await build(options);
  console.log('simulator: built');
}
