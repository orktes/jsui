// `jsui build <entry> [-o dist/bundle.js] [--minify] [--sourcemap] [--react-compiler]`

import * as esbuild from 'esbuild';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { esbuildOptions } from './config.js';
import { REACT_COMPILER_DEFAULT } from './react-compiler.js';

export async function formatErrors(errors) {
  const lines = await esbuild.formatMessages(errors, { kind: 'error', color: process.stderr.isTTY });
  return lines.join('\n');
}

/**
 * Builds a QuickJS-ready IIFE bundle and writes it plus its assets.
 * `reactCompiler` runs React Compiler on the app's own sources (see
 * react-compiler.js); `reactCompiler.skipped` lists the components and hooks
 * it left uncompiled, with the reason.
 * @returns {Promise<{outfile: string, bytes: number, assets: string[], warnings: object[],
 *   reactCompiler: null | {files: number, functions: number, skipped: object[]}}>}
 */
export async function build({ entry, outfile = 'dist/bundle.js', minify = false, sourcemap = false, mode = 'production', reactCompiler = REACT_COMPILER_DEFAULT }) {
  const assets = new Map();
  const absOut = path.resolve(outfile);
  const report = reactCompiler ? { files: 0, functions: 0, skipped: [] } : null;
  let result;
  try {
    result = await esbuild.build(esbuildOptions({ entry: path.resolve(entry), outfile: absOut, mode, minify, sourcemap, assets, reactCompiler, compilerReport: report }));
  } catch (e) {
    if (e && e.errors) {
      const err = new Error(await formatErrors(e.errors));
      err.esbuildErrors = e.errors;
      throw err;
    }
    throw e;
  }
  const outDir = path.dirname(absOut);
  await mkdir(outDir, { recursive: true });
  let bytes = 0;
  for (const f of result.outputFiles) {
    await mkdir(path.dirname(f.path), { recursive: true });
    await writeFile(f.path, f.contents);
    if (f.path === absOut) bytes = f.contents.length;
  }
  for (const [rel, buf] of assets) {
    const p = path.join(outDir, rel);
    await mkdir(path.dirname(p), { recursive: true });
    await writeFile(p, buf);
  }
  return { outfile: absOut, bytes, assets: [...assets.keys()], warnings: result.warnings, reactCompiler: report };
}
