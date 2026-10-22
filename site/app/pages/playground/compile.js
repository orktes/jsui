// Compiles playground code INSIDE the runtime: Sucrase (bundled into the
// site, running on QuickJS) turns JSX + ES imports into CommonJS, then the
// module body is evaluated with `new Function` and a require() that hands
// out the real React and react-native modules of this bundle.
import React from 'react';
import * as ReactNative from 'react-native';
import { transform } from 'sucrase';

const RN = Object.assign({}, ReactNative, { __esModule: true, default: ReactNative });
const MODULES = {
  react: React,
  'react-native': RN,
  '@jsui/react': RN,
};

function requireModule(name) {
  const m = MODULES[name];
  if (!m) throw new Error(`Cannot find module '${name}' (the playground provides 'react' and 'react-native')`);
  return m;
}

/**
 * Compiles and evaluates `source`.
 * Returns { ok: true, Component, ms } or
 * { ok: false, kind: 'compile' | 'runtime', message, line, column, ms }.
 */
export function compile(source) {
  const t0 = Date.now();
  let code;
  try {
    code = transform(source, { transforms: ['jsx', 'imports'], production: true, jsxRuntime: 'classic' }).code;
  } catch (e) {
    const loc = e.loc || {};
    return {
      ok: false,
      kind: 'compile',
      message: String(e.message || e).replace(/\s*\(\d+:\d+\)$/, ''),
      line: loc.line,
      column: loc.column,
      ms: Date.now() - t0,
    };
  }
  const module = { exports: {} };
  try {
    // eslint-disable-next-line no-new-func
    const fn = new Function('React', 'require', 'module', 'exports', code);
    fn(React, requireModule, module, module.exports);
  } catch (e) {
    return { ok: false, kind: e instanceof SyntaxError ? 'compile' : 'runtime', message: String((e && e.message) || e), ms: Date.now() - t0 };
  }
  const ex = module.exports;
  const Component = (ex && ex.default) || (typeof ex === 'function' ? ex : null) || (ex && ex.App);
  if (typeof Component !== 'function' && !(Component && Component.$$typeof)) {
    return { ok: false, kind: 'runtime', message: 'Export a component: export default function App() { ... }', ms: Date.now() - t0 };
  }
  return { ok: true, Component, ms: Date.now() - t0 };
}
