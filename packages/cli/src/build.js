// `jsui build <entry> [-o dist/bundle.js] [--minify] [--sourcemap] [--no-bytecode]`

import * as esbuild from 'esbuild';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { compileBundle } from './bytecode.js';
import { esbuildOptions } from './config.js';

export async function formatErrors(errors) {
  const lines = await esbuild.formatMessages(errors, { kind: 'error', color: process.stderr.isTTY });
  return lines.join('\n');
}

/**
 * Builds a QuickJS-ready IIFE bundle and writes it plus its assets.
 * `bytecode` precompiles it to QuickJS bytecode (src/js/bytecode.h: faster
 * startup, less memory; the source stays inside as the fallback for other
 * runtime versions). Without the WebAssembly runtime the bundle stays
 * source and `bytecode` in the result says why.
 * @returns {Promise<{outfile: string, bytes: number, sourceBytes: number, bytecode: true | string | false, assets: string[], warnings: object[]}>}
 */
export async function build({ entry, outfile = 'dist/bundle.js', minify = false, sourcemap = false, mode = 'production', bytecode = false, wasmDir }) {
  const assets = new Map();
  const absOut = path.resolve(outfile);
  let result;
  try {
    result = await esbuild.build(esbuildOptions({ entry: path.resolve(entry), outfile: absOut, mode, minify, sourcemap, assets }));
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
  let sourceBytes = 0;
  let compiled = false;
  for (const f of result.outputFiles) {
    let contents = f.contents;
    if (f.path === absOut) {
      sourceBytes = contents.length;
      if (bytecode) {
        try {
          contents = await compileBundle(contents, { name: path.basename(absOut), wasmDir });
          compiled = true;
        } catch (e) {
          if (e.code !== 'JSUI_NO_WASM') throw new Error(`${path.basename(absOut)}: ${e.message}`);
          compiled = `kept as source: ${e.message.split('\n')[0]}`;
        }
      }
      bytes = contents.length;
    }
    await mkdir(path.dirname(f.path), { recursive: true });
    await writeFile(f.path, contents);
  }
  for (const [rel, buf] of assets) {
    const p = path.join(outDir, rel);
    await mkdir(path.dirname(p), { recursive: true });
    await writeFile(p, buf);
  }
  return { outfile: absOut, bytes, sourceBytes, bytecode: compiled, assets: [...assets.keys()], warnings: result.warnings };
}
