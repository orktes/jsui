// Production bundles specialize react-reconciler for @orktes/jsui-react's
// host config (packages/react/src/renderer.js). react-reconciler is a generic
// factory: it reads every host config entry into a closure variable and
// tests the feature flags (supportsMutation, supportsHydration, ...) and
// calls the hooks (shouldSetTextContent, ...) on every fiber. The jsui host
// config's flags and some of its hooks are constants, so the copy of the
// reconciler that renderer.js imports gets them written in as literals
// (esbuild's minifySyntax then drops the dead branches), and the fragment
// ref bookkeeping, which our host config makes a no-op (createFragmentInstance
// returns null, so no fiber ever holds a fragment instance), is removed.
//
// Only renderer.js's import is specialized: any other importer of
// react-reconciler (an app's own custom renderer) gets it as published.
// Each patch must match the installed reconciler exactly as many times as
// it expects, or the reconciler is bundled unpatched (test/specialize.test.js
// checks that every patch applies to the installed version).

import { readFile } from 'node:fs/promises';
import path from 'node:path';

/** Host config entries that are constants in renderer.js (keep in sync; checked by the tests). */
export const HOST_FLAGS = {
  supportsMutation: true,
  supportsPersistence: false,
  supportsHydration: false,
  supportsMicrotasks: true,
  supportsTestSelectors: false,
  supportsResources: false,
  supportsSingletons: false,
  isPrimaryRenderer: true,
};

// [name, regexp (global), replacement, expected match count]
function patches() {
  const list = [];
  for (const [name, value] of Object.entries(HOST_FLAGS)) {
    const lit = value ? '!0' : '!1';
    // The declaration `name = $$$config.name` keeps reading the config (under
    // another name) so the var list stays valid; every other use is the literal.
    list.push([`${name} declaration`, new RegExp(`(?<![\\w$.])${name} = \\$\\$\\$config\\.${name}\\b`, 'g'), `__jsui_${name} = $$$$$$config.${name}`, 1]);
    list.push([`${name} uses`, new RegExp(`(?<![\\w$.])${name}(?![\\w$])`, 'g'), lit, -1]);
  }
  // shouldSetTextContent: () => false. Called twice per host fiber render.
  list.push(['shouldSetTextContent', /(?<![\w$.])shouldSetTextContent\([^()]*\)/g, '!1', 4]);
  // getPublicInstance: (instance) => instance.
  list.push(['getPublicInstance', /(?<![\w$.])getPublicInstance\(([\w$.]+)\)/g, '$1', 5]);
  // Fragment refs: no fiber has a fragment instance, so the walks up to the
  // host parent that look for one find nothing.
  list.push([
    'fragment instance effects',
    /(?<![\w$.]|function )commitFragmentInstance(?:Deletion|Insertion)Effects\([\w$]+\)/g,
    'void 0',
    7,
  ]);
  list.push([
    'placement fragment instances',
    /for \(var parent = finishedWork\.return; null !== parent; \) \{\n\s*if \(isFragmentInstanceParent\(parent\)\) \{\n\s*var fragmentInstance = parent\.stateNode;\n\s*null === parentFiber\n\s*\? \(parentFiber = \[fragmentInstance\]\)\n\s*: parentFiber\.push\(fragmentInstance\);\n\s*\}\n\s*if \(isFragmentInstanceHostBoundary\(parent\)\) break;\n\s*parent = parent\.return;\n\s*\}/g,
    '',
    1,
  ]);
  return list;
}

/**
 * Applies the patches to the reconciler source. Returns {code, applied,
 * failed}: with any failure the source comes back unchanged.
 */
export function specializeReconciler(source) {
  let code = source;
  const applied = [];
  const failed = [];
  for (const [name, re, rep, expected] of patches()) {
    const n = (code.match(re) || []).length;
    if (expected >= 0 ? n !== expected : n === 0) {
      failed.push(`${name}: ${n} matches, expected ${expected >= 0 ? expected : 'some'}`);
      continue;
    }
    code = code.replace(re, rep);
    applied.push(name);
  }
  if (failed.length) return { code: source, applied: [], failed };
  return { code, applied, failed };
}

const RECONCILER_PROD = /react-reconciler[\\/]cjs[\\/]react-reconciler\.production\.js$/;

/**
 * esbuild plugin: renderer.js's `import Reconciler from 'react-reconciler'`
 * resolves to the specialized production build. Production mode only.
 */
export function specializePlugin({ reactDir, log }) {
  const renderer = path.join(reactDir, 'src', 'renderer.js');
  return {
    name: 'jsui-specialize-reconciler',
    setup(build) {
      build.onResolve({ filter: /^react-reconciler$/ }, async (args) => {
        if (args.importer !== renderer || (args.pluginData && args.pluginData.jsuiSpecialize)) return undefined;
        const r = await build.resolve('react-reconciler/cjs/react-reconciler.production.js', {
          kind: args.kind,
          resolveDir: reactDir,
          pluginData: { jsuiSpecialize: true, jsuiAlias: true },
        });
        if (r.errors.length) return { errors: r.errors };
        return { path: r.path, suffix: '?jsui-host', sideEffects: r.sideEffects };
      });
      build.onLoad({ filter: RECONCILER_PROD }, async (args) => {
        if (args.suffix !== '?jsui-host') return undefined;
        const source = await readFile(args.path, 'utf8');
        const { code, failed } = specializeReconciler(source);
        if (failed.length && log) log(`jsui: react-reconciler bundled unspecialized (${failed[0]})`);
        return { contents: code, loader: 'js', resolveDir: path.dirname(args.path) };
      });
    },
  };
}
