// Worklets at build time (react-native-worklets; docs/ARCHITECTURE.md
// "Worklets").
//
// A worklet runs in a worklet runtime of its own (a separate QuickJS
// context on JSUI's UI thread), so the runtime needs its source text and the
// values it captures. Bytecode bundles keep no function source
// (Function#toString gives "[native code]"), so, as in React Native, the
// build does it: Software Mansion's Babel plugin (vendor/
// worklets-babel-plugin.cjs, run on Babel 7) rewrites every function with a
// 'worklet' directive, and the callbacks passed straight to runOnUI /
// scheduleOnUI / runOnRuntime / ... (its autoworkletization), into
//
//   const foo = function fooFactory({ _worklet_123_init_data, a, b }) {
//     const foo = function (x) { ... };
//     foo.__closure = [a, b];
//     foo.__workletHash = 123;
//     foo.__initData = _worklet_123_init_data;  // { code, location, line }
//     return foo;
//   }({ _worklet_123_init_data, a, b });
//
// with `code` the worklet as a function that reads its captured values from
// `this.__closure`. @orktes/jsui-react's react-native-worklets sends that
// object to the runtime (src/worklets.js).
//
// Only files that mention "worklet" are parsed (an import of
// react-native-worklets does): it runs on app files and packages alike,
// before React Compiler (whose memoization must not reach into a worklet)
// and React Refresh. TypeScript is stripped first (esbuild), so the code a
// worklet carries is plain JS.

import * as esbuild from 'esbuild';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);

let babel7 = null;
let plugin = null;
function load() {
  if (!babel7) {
    babel7 = require('babel-core-7');
    plugin = require('./vendor/worklets-babel-plugin.cjs');
  }
}

// Line in the stripped output -> line in the TypeScript source.
function sourceLines(map) {
  const tm = createRequire(require.resolve('babel-core-7'))('@jridgewell/trace-mapping');
  const traced = new tm.TraceMap(map);
  return (line) => {
    const p = tm.originalPositionFor(traced, { line, column: 0, bias: tm.LEAST_UPPER_BOUND });
    return p.line || line;
  };
}

/** True when the transform may change `src` (cheap pre-check). */
export function mayHaveWorklets(src) {
  return src.includes('worklet');
}

/**
 * Workletizes `src`.
 * @param {string} src
 * @param {string} file  absolute path (messages, `location`, the loader)
 * @param {object} [o]
 * @param {'development'|'production'} [o.mode]  production leaves out the
 *   worklets' location and dev stack data (react-native-worklets' release
 *   builds)
 * @param {boolean} [o.sourcemap]  append an inline source map (to `src`)
 * @returns {Promise<{code: string, changed: boolean, loader?: string}>}
 *   `loader`: what esbuild should load `code` as (TypeScript is gone)
 */
export async function transformWorklets(src, file, o = {}) {
  if (!mayHaveWorklets(src)) return { code: src, changed: false };
  load();
  const ext = path.extname(file).slice(1).toLowerCase();
  let input = src;
  let inputMap = false;
  let lineMap;
  if (ext === 'ts' || ext === 'tsx' || ext === 'mts' || ext === 'cts') {
    const r = await esbuild.transform(src, {
      loader: ext === 'tsx' ? 'tsx' : 'ts',
      jsx: 'preserve',
      target: 'esnext',
      sourcefile: file,
      sourcemap: 'external',  // (also for the worklets' line numbers)
      logLevel: 'silent',
    });
    input = r.code;
    const map = JSON.parse(r.map);
    if (o.sourcemap) inputMap = map;
    lineMap = sourceLines(map);
  }
  const r = await babel7.transformAsync(input, {
    filename: file,
    cwd: process.cwd(),
    envName: o.mode === 'production' ? 'production' : 'development',
    babelrc: false,
    configFile: false,
    sourceType: 'unambiguous',
    parserOpts: { plugins: ['jsx'] },
    plugins: [[plugin, {
      // The worklets' own source maps are not used (errors name the file and line).
      disableSourceMaps: true,
      relativeSourceLocation: true,
      disableWorkletClasses: true,
      disableInlineStylesWarning: true,
      jsuiLineMap: lineMap,
    }]],
    compact: false,
    // Without a source map, keep the app's line numbers (stack traces).
    retainLines: !o.sourcemap,
    sourceMaps: o.sourcemap ? 'inline' : false,
    inputSourceMap: inputMap,
  });
  // Nothing workletized (the file only mentions the word): as written.
  if (!r.code.includes('__workletHash')) return { code: src, changed: false };
  return { code: r.code, changed: true, loader: 'jsx' };
}

/**
 * The build-time passes every source file goes through before React
 * Compiler / React Refresh / esbuild: Animated.derive (derive.js), then
 * worklets.
 * @returns {Promise<{code: string, changed: boolean, warnings: object[], loader: string}>}
 *   `loader`: esbuild's loader for `code`
 */
export async function prepareSource(src, file, o = {}) {
  const { transformDerive } = await import('./derive.js');
  const d = await transformDerive(src, file);
  const w = await transformWorklets(d.code, file, o);
  const ext = path.extname(file).slice(1).toLowerCase();
  const loader = w.loader || (ext === 'ts' || ext === 'tsx' ? ext : ext === 'json' ? 'json' : 'jsx');
  return { code: w.code, changed: w.changed || d.code !== src, warnings: d.warnings, loader };
}
