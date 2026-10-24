// `jsui build <entry> [-o dist/bundle.js] [--minify] [--sourcemap] [--format combo|bytecode|source|split]`

import * as esbuild from 'esbuild';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { compileBundle } from './bytecode.js';
import { esbuildOptions } from './config.js';

export async function formatErrors(errors) {
  const lines = await esbuild.formatMessages(errors, { kind: 'error', color: process.stderr.isTTY });
  return lines.join('\n');
}

export const BUNDLE_FORMATS = ['combo', 'bytecode', 'source', 'split'];

/** `dist/bundle.js` -> `dist/bundle.source.js`: the source file of a split bundle. */
export function splitSourcePath(outfile) {
  const ext = path.extname(outfile);
  return outfile.slice(0, outfile.length - ext.length) + '.source' + (ext || '.js');
}

/**
 * Builds a QuickJS-ready IIFE bundle and writes it plus its assets.
 * `format` (src/js/bytecode.h has the container):
 *   'source'   plain JS (the default here; `jsui build` defaults to 'combo')
 *   'combo'    QuickJS bytecode plus the source inside as the fallback for
 *              runtimes of other JSUI versions
 *   'bytecode' bytecode only (smallest; only this JSUI version loads it)
 *   'split'    bytecode, with the source next to it in <name>.source.js
 *              (the fallback; the runtime reads it only when it needs it)
 * Bytecode is compiled by the WebAssembly runtime; without it the bundle
 * stays source and `bytecode` in the result says why.
 * @returns {Promise<{outfile: string, sourceFile: string | null, bytes: number, sourceBytes: number, format: string, bytecode: true | string | false, assets: string[], warnings: object[]}>}
 */
export async function build({ entry, outfile = 'dist/bundle.js', minify = false, sourcemap = false, mode = 'production', format = 'source', wasmDir }) {
  if (!BUNDLE_FORMATS.includes(format)) throw new Error(`unknown bundle format "${format}" (${BUNDLE_FORMATS.join(', ')})`);
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
  let sourceFile = null;
  for (const f of result.outputFiles) {
    let contents = f.contents;
    if (f.path === absOut) {
      sourceBytes = contents.length;
      if (format !== 'source') {
        const split = format === 'split' ? splitSourcePath(absOut) : null;
        try {
          contents = await compileBundle(contents, {
            name: path.basename(absOut),
            keepSource: format === 'combo',
            sourceUrl: split && path.basename(split),
            wasmDir,
          });
          compiled = true;
        } catch (e) {
          if (e.code !== 'JSUI_NO_WASM') throw new Error(`${path.basename(absOut)}: ${e.message}`);
          compiled = `kept as source: ${e.message.split('\n')[0]}`;
        }
        if (compiled === true && split) {
          await mkdir(path.dirname(split), { recursive: true });
          await writeFile(split, f.contents);
          sourceFile = split;
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
  return { outfile: absOut, sourceFile, bytes, sourceBytes, format: compiled === true ? format : 'source', bytecode: compiled, assets: [...assets.keys()], warnings: result.warnings };
}
