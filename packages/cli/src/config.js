// esbuild configuration shared by `jsui build` and `jsui dev`.

import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { assetPlugin } from './assets.js';
import { reactCompilerPlugin } from './react-compiler.js';
import { derivePlugin } from './derive.js';
import { schedulerPlugin, specializePlugin } from './specialize.js';

const require = createRequire(import.meta.url);

/** Directory of the installed @orktes/jsui-react package. */
export function jsuiReactDir() {
  return path.dirname(require.resolve('@orktes/jsui-react/package.json'));
}

// Popular RN ecosystem packages that JSUI provides drop-in shims for.
const SHIMS = {
  'react-native-safe-area-context': 'safe-area-context',
  'expo-status-bar': 'expo-status-bar',
  'expo-linear-gradient': 'linear-gradient',
  'react-native-linear-gradient': 'linear-gradient',
  'lottie-react-native': 'lottie-react-native',
  'expo-font': 'expo-font',
  'react-native-worklets': 'react-native-worklets',
};

// These must resolve to the single copy @orktes/jsui-react was built against, or
// hooks break (two Reacts) and the reconciler/react versions mismatch.
const SINGLETONS = /^(react|react\/jsx-runtime|react\/jsx-dev-runtime|react\/compiler-runtime|react-reconciler|react-reconciler\/constants|scheduler)$/;

// React's packages are CommonJS. esbuild hands an ESM `import { useState }
// from 'react'` a copy of module.exports whose every property is a getter
// (__toESM), so each `useState(...)` and each JSX element (`jsx(...)` from
// react/jsx-runtime) called a getter function first: one more JS call per
// hook and per element in QuickJS. ESM imports of these modules get a facade
// instead that copies the exports once into plain ESM bindings, which
// esbuild then references directly; `export *` keeps names React does not
// export building (undefined at runtime, as before). require() still gets
// module.exports. Production only (the HMR vendor bundle of `jsui dev`
// resolves these modules by path).
const ESM_FACADES = /^react(\/jsx-runtime|\/jsx-dev-runtime|\/compiler-runtime)?$/;
const facadeKeys = new Map();

/** Every export name of a CommonJS entry, in development and production. */
function exportNames(file) {
  let names = facadeKeys.get(file);
  if (names) return names;
  const set = new Set();
  const add = (f) => {
    for (const k of Object.keys(require(f))) set.add(k);
  };
  // The entry picks its build by NODE_ENV: read both.
  const src = fs.readFileSync(file, 'utf8');
  const builds = [...src.matchAll(/require\(['"](\.\/cjs\/[^'"]+)['"]\)/g)].map((m) => path.join(path.dirname(file), m[1]));
  if (builds.length) builds.forEach(add);
  else add(file);
  set.delete('default');
  set.delete('__esModule');
  names = [...set].filter((k) => /^[A-Za-z_$][\w$]*$/.test(k)).sort();
  facadeKeys.set(file, names);
  return names;
}

function esmFacade(file) {
  const names = exportNames(file);
  return (
    `import M from ${JSON.stringify(file)};\nexport default M;\nexport * from ${JSON.stringify(file)};\n` +
    (names.length ? `export var ${names.map((k) => `${k} = M.${k}`).join(',\n  ')};\n` : '')
  );
}

function aliasPlugin({ facades = false } = {}) {
  return {
    name: 'jsui-alias',
    setup(build) {
      const reactDir = jsuiReactDir();
      const index = path.join(reactDir, 'src', 'index.js');
      build.onResolve({ filter: /^react-native(\/.*)?$/ }, () => ({ path: index }));
      // The app may not depend on @orktes/jsui-react itself (only on the CLI).
      build.onResolve({ filter: /^@orktes\/jsui-react$/ }, () => ({ path: index }));
      build.onResolve({ filter: /^[@a-z]/ }, async (args) => {
        if (args.pluginData && args.pluginData.jsuiAlias) return undefined;
        const shim = SHIMS[args.path];
        if (shim) return { path: path.join(reactDir, 'src', 'shims', shim + '.js') };
        if (!SINGLETONS.test(args.path)) return undefined;
        const r = await build.resolve(args.path, { kind: args.kind, resolveDir: reactDir, pluginData: { jsuiAlias: true } });
        if (r.errors.length) return { errors: r.errors };
        // (The facade's path is only its name in the bundle: no local paths.)
        if (facades && args.kind === 'import-statement' && ESM_FACADES.test(args.path)) return { path: args.path, namespace: 'jsui-esm', pluginData: { file: r.path } };
        return { path: r.path, sideEffects: r.sideEffects };
      });
      build.onLoad({ filter: /.*/, namespace: 'jsui-esm' }, (args) => ({ contents: esmFacade(args.pluginData.file), loader: 'js', resolveDir: path.dirname(args.pluginData.file) }));
    },
  };
}

/**
 * @param {object} o
 * @param {string} o.entry
 * @param {string} o.outfile
 * @param {'development'|'production'} o.mode
 * @param {boolean} [o.minify]
 * @param {boolean|'inline'} [o.sourcemap]
 * @param {Map<string, Buffer>} o.assets
 * @param {boolean} [o.reactCompiler]  run React Compiler on the app's sources
 * @param {object} [o.compilerReport]  filled with what it compiled and skipped
 * @param {boolean} [o.compilerHmr]  compile for hot updates (jsui dev with HMR)
 * @param {(msg: string) => void} [o.log]
 */
export function esbuildOptions(o) {
  const dev = o.mode === 'development';
  return {
    entryPoints: [o.entry],
    outfile: o.outfile,
    bundle: true,
    write: false,
    format: 'iife',
    platform: 'browser',
    target: 'es2020',
    charset: 'utf8',
    minify: !!o.minify,
    // Production bundles always get esbuild's syntax minification (constant
    // folding, dead branches, shorter expression forms) but keep names and
    // whitespace: less bytecode for QuickJS to run (jsui-bench: -1 to -4%
    // Ir/frame on the update scenarios) with readable stacks.
    minifySyntax: !!o.minify || !dev,
    sourcemap: o.sourcemap === 'inline' ? 'inline' : o.sourcemap ? 'linked' : false,
    legalComments: o.minify ? 'none' : 'inline',
    jsx: 'automatic',
    jsxImportSource: 'react',
    jsxDev: false,
    loader: { '.js': 'jsx', '.json': 'json' },
    resolveExtensions: [
      '.jsui.tsx', '.jsui.ts', '.jsui.jsx', '.jsui.js',
      '.native.tsx', '.native.ts', '.native.jsx', '.native.js',
      '.tsx', '.ts', '.jsx', '.js', '.mjs', '.cjs', '.json',
    ],
    mainFields: ['browser', 'module', 'main'],
    define: {
      'process.env.NODE_ENV': JSON.stringify(o.mode),
      __DEV__: String(dev),
      global: 'globalThis',
    },
    // Libraries sometimes read process.* beyond NODE_ENV.
    banner: { js: `var process = globalThis.process || { env: { NODE_ENV: ${JSON.stringify(o.mode)} }, platform: "jsui", nextTick: function (f) { Promise.resolve().then(f); } };` },
    logLevel: 'silent',
    plugins: [
      schedulerPlugin({ log: o.log }),
      // Before the alias plugin: it claims renderer.js's react-reconciler import.
      ...(dev ? [] : [specializePlugin({ reactDir: jsuiReactDir(), log: o.log })]),
      aliasPlugin({ facades: !dev }),
      assetPlugin(o.assets),
      ...(o.reactCompiler ? [reactCompilerPlugin({ reactDir: jsuiReactDir(), mode: o.mode, sourcemap: !!o.sourcemap, hmr: !!o.compilerHmr, report: o.compilerReport, log: o.log })] : []),
      // After the React Compiler (which applies the same transform first).
      derivePlugin({ reactDir: jsuiReactDir(), mode: o.mode, sourcemap: !!o.sourcemap }),
    ],
    metafile: true,
  };
}
