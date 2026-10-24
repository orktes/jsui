// esbuild configuration shared by `jsui build` and `jsui dev`.

import { createRequire } from 'node:module';
import path from 'node:path';
import { assetPlugin } from './assets.js';
import { reactCompilerPlugin } from './react-compiler.js';
import { specializePlugin } from './specialize.js';

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
};

// These must resolve to the single copy @orktes/jsui-react was built against, or
// hooks break (two Reacts) and the reconciler/react versions mismatch.
const SINGLETONS = /^(react|react\/jsx-runtime|react\/jsx-dev-runtime|react\/compiler-runtime|react-reconciler|react-reconciler\/constants|scheduler)$/;

function aliasPlugin() {
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
        return { path: r.path, sideEffects: r.sideEffects };
      });
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
      // Before the alias plugin: it claims renderer.js's react-reconciler import.
      ...(dev ? [] : [specializePlugin({ reactDir: jsuiReactDir(), log: o.log })]),
      aliasPlugin(),
      assetPlugin(o.assets),
      ...(o.reactCompiler ? [reactCompilerPlugin({ reactDir: jsuiReactDir(), sourcemap: !!o.sourcemap, hmr: !!o.compilerHmr, report: o.compilerReport, log: o.log })] : []),
    ],
    metafile: true,
  };
}
