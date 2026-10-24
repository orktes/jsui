// esbuild configuration shared by `jsui build` and `jsui dev`.

import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { assetPlugin } from './assets.js';

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
const SINGLETONS = /^(react|react\/jsx-runtime|react\/jsx-dev-runtime|react-reconciler|react-reconciler\/constants|scheduler)$/;

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

// Small, exact source patches to react-reconciler's production build for
// QuickJS (no JIT: every loop iteration of React's own bookkeeping is paid
// in the interpreter). Each patch is applied only when its anchor matches
// exactly once, so another React version builds unpatched rather than
// wrongly. JSUI_REACT_PATCHES=0 turns them off (A/B measurements).
const REACT_PATCHES = [
  {
    // Lazy context propagation: every fiber that bails out walks up to the
    // root looking for context providers whose value changed, stopping at a
    // fiber that already did (DidPropagateContext, 262144). Siblings never
    // stop early, so 500 memoized list rows that bail out walk the whole
    // ancestor chain 500 times (17% of an inventory update's JS). When the
    // walk found no changed provider, the parent's walk (a suffix of it)
    // would find none either: mark it, so the siblings stop there.
    file: /react-reconciler[\\/]cjs[\\/]react-reconciler\.production\.js$/,
    from: '    workInProgress.flags |= 262144;\n    return null !== current;\n',
    to: '    workInProgress.flags |= 262144;\n    null === current && null !== workInProgress.return && (workInProgress.return.flags |= 262144);\n    return null !== current;\n',
  },
];

function reactPatchPlugin() {
  return {
    name: 'jsui-react-patches',
    setup(build) {
      if (process.env.JSUI_REACT_PATCHES === '0') return;
      build.onLoad({ filter: /react-reconciler[\\/]cjs[\\/].*\.js$/ }, async (args) => {
        const patches = REACT_PATCHES.filter((p) => p.file.test(args.path));
        if (!patches.length) return undefined;
        let src = await fs.promises.readFile(args.path, 'utf8');
        for (const p of patches) {
          const i = src.indexOf(p.from);
          if (i < 0 || src.indexOf(p.from, i + 1) >= 0) continue;
          src = src.slice(0, i) + p.to + src.slice(i + p.from.length);
        }
        return { contents: src, loader: 'js' };
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
    plugins: [aliasPlugin(), reactPatchPlugin(), assetPlugin(o.assets)],
    metafile: true,
  };
}
