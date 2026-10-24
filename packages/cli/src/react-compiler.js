// React Compiler (babel-plugin-react-compiler) for app source files.
//
// The compiler memoizes components and hooks automatically (as if every
// derived value, callback and JSX element were wrapped in useMemo /
// useCallback), so a re-render reuses the elements whose inputs did not
// change and React skips those subtrees. It runs on app files only: packages
// in node_modules ship their own (compiled or not) code, and
// @orktes/jsui-react's components are hand-tuned for the bridge.
//
// The compiled code imports `react/compiler-runtime` (part of React 19; the
// alias plugin of config.js resolves it to the singleton React).
//
// The compiler runs on Babel 7 (`babel-core-7`, an alias of @babel/core 7):
// it is written against Babel 7's AST, and under Babel 8 it bails out of
// every function with a defaulted destructured parameter
// (`function App({ size = 1 })`, "Expected object property value to be an
// LVal"). React Refresh (hmr.js) stays on Babel 8.
//
// Functions the compiler cannot prove safe (they break the Rules of React,
// e.g. read ref.current during render) are left as written and reported:
// the app still works, those components only miss the memoization.

import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { realpathSync } from 'node:fs';
import { createRequire } from 'node:module';
import { availableParallelism } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker } from 'node:worker_threads';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));

/**
 * Whether `jsui build` / `jsui dev` / the headless commands compile by
 * default: no (opt in with --react-compiler / `reactCompiler: true`). It
 * halves the per-frame JS of UIs whose parents re-render (the showcase), but
 * costs ~10% where every rendered component's props change each frame (the
 * markers bench), builds 4-25x slower, and code that reads mutable state
 * during render (outside hooks) can show stale values once memoized; see
 * docs/ARCHITECTURE.md ("Performance", React Compiler).
 */
export const REACT_COMPILER_DEFAULT = false;

export const SOURCE_EXT = /\.(jsx?|tsx?|mjs|cjs)$/i;

let babel7 = null;
let compilerPlugin = null;
function load() {
  if (!babel7) {
    babel7 = require('babel-core-7');
    compilerPlugin = require('babel-plugin-react-compiler');
  }
}

function real(p) {
  try {
    return realpathSync.native(p);
  } catch {
    return p;
  }
}

/**
 * True for the app's own source files: not in node_modules, not
 * @orktes/jsui-react's sources (src/) and not the CLI's (the HMR runtime).
 */
export function appSourceFilter(reactDir) {
  const skip = [real(path.join(reactDir, 'src')) + path.sep, real(path.dirname(here)) + path.sep];
  return (abs) => {
    if (!SOURCE_EXT.test(abs)) return false;
    const p = real(abs);
    return !p.includes(`${path.sep}node_modules${path.sep}`) && !skip.some((d) => p.startsWith(d));
  };
}

/** Babel parser plugins for a source file (JSX is allowed in .js, as in config.js). */
export function parserPlugins(file) {
  const ext = path.extname(file).slice(1).toLowerCase();
  if (ext === 'ts') return ['typescript'];
  if (ext === 'tsx') return ['typescript', 'jsx'];
  return ['jsx'];
}

function reasonOf(event) {
  if (event.kind === 'PipelineError') return `compiler error: ${String(event.data).split('\n')[0]}`;
  if (event.kind === 'CompileSkip') return event.reason;
  const d = detailOf(event);
  const text = d.reason || d.message || 'unknown reason';
  const first = d.details && d.details.find((x) => x.message);
  return first && first.message !== text ? `${text} (${first.message})` : text;
}

// CompilerErrorDetail or CompilerDiagnostic (whose locations are in details[]).
function detailOf(event) {
  return event.detail ? event.detail.options || event.detail : {};
}

function locOf(event) {
  const d = detailOf(event);
  if (d.loc && d.loc.start) return d.loc;
  const first = d.details && d.details.find((x) => x.loc && x.loc.start);
  return first ? first.loc : null;
}

// The compiler names only the functions it compiled; for the others, the
// name is read off the source line the function starts on.
function nameAt(lines, loc) {
  const text = loc && loc.start ? lines[loc.start.line - 1] || '' : '';
  const m = /(?:function\s*\*?\s*|(?:const|let|var)\s+|^\s*)([A-Za-z_$][\w$]*)\s*(?:=|\(|:)/.exec(text.slice(loc ? loc.start.column : 0)) ||
    /(?:function\s+|(?:const|let|var)\s+)([A-Za-z_$][\w$]*)/.exec(text);
  return m ? m[1] : null;
}

/** compileSource() on this thread (react-compiler-worker.js runs it). */
export async function compileInProcess(source, file, o = {}) {
  load();
  const lines = source.split('\n');
  const rel = path.relative(process.cwd(), file);
  const skipped = [];
  const seen = new Set();
  let functions = 0;
  const logger = {
    logEvent(_filename, event) {
      if (event.kind === 'CompileSuccess') {
        functions++;
        return;
      }
      if (event.kind !== 'CompileError' && event.kind !== 'PipelineError' && event.kind !== 'CompileSkip') return;
      const at = locOf(event);
      const loc = event.fnLoc || at || null;
      const where = rel + (loc && loc.start ? `:${loc.start.line}:${loc.start.column + 1}` : '');
      if (seen.has(where)) return;  // one function can report several errors
      seen.add(where);
      skipped.push({
        name: event.fnName || nameAt(lines, loc),
        where,
        line: at && at.start ? at.start.line : null,  // of the offending code
        reason: reasonOf(event),
      });
    },
  };
  const r = await babel7.transformAsync(source, {
    filename: file,
    babelrc: false,
    configFile: false,
    sourceType: 'unambiguous',
    parserOpts: { plugins: parserPlugins(file) },
    plugins: [[compilerPlugin, {
      target: '19',
      panicThreshold: 'none',
      // Explicit: the compiler would otherwise read the CLI's NODE_ENV.
      environment: { enableResetCacheOnSourceFileChanges: !!o.hmr },
      logger,
    }]],
    compact: false,
    retainLines: false,
    sourceMaps: o.sourcemap ? 'inline' : o.map ? true : false,
    ...(o.map && !o.sourcemap ? { sourceFileName: file } : {}),
  });
  return { code: r.code, map: o.map && !o.sourcemap ? r.map || null : null, functions, skipped };
}

// --- worker pool ----------------------------------------------------------
// The compiler is slow (tens of ms per KB of source, on one thread), so files
// compile on worker threads, up to 4 at a time. Idle workers do not keep the
// process alive.

const pool = [];  // { worker, jobs: Map(id -> {resolve, reject}) }
let nextJob = 1;
const POOL_SIZE = Math.max(1, Math.min(4, availableParallelism()));

function spawn() {
  const slot = { worker: new Worker(path.join(here, 'react-compiler-worker.js')), jobs: new Map() };
  slot.worker.on('message', (m) => {
    const job = slot.jobs.get(m.id);
    slot.jobs.delete(m.id);
    if (!slot.jobs.size) slot.worker.unref();
    if (m.error) job.reject(Object.assign(new Error(m.error), { stack: m.stack }));
    else job.resolve(m.result);
  });
  slot.worker.on('error', (e) => {
    pool.splice(pool.indexOf(slot), 1);
    for (const job of slot.jobs.values()) job.reject(e);
    slot.jobs.clear();
  });
  pool.push(slot);
  return slot;
}

function inWorker(source, file, o) {
  let slot = pool.reduce((a, b) => (b.jobs.size < a.jobs.size ? b : a), pool[0] || null);
  if (!slot || (slot.jobs.size && pool.length < POOL_SIZE)) slot = spawn();
  const id = nextJob++;
  slot.worker.ref();
  slot.worker.postMessage({ id, source, file, o });
  return new Promise((resolve, reject) => slot.jobs.set(id, { resolve, reject }));
}

const results = new Map();  // file + options -> { hash, promise } (jsui dev rebuilds, HMR)

/**
 * Runs the compiler on one source file (cached by content: jsui dev's bundle
 * build and its HMR modules share the result).
 * @param {string} source
 * @param {string} file  absolute path (for the parser and messages)
 * @param {object} [o]
 * @param {boolean} [o.hmr]  reset a component's memo cache when its file is
 *   hot-updated (the compiler adds a source hash check)
 * @param {boolean} [o.sourcemap]  append an inline source map
 * @param {boolean} [o.map]  return the source map as `map` (an object; sources
 *   are the absolute path of `file`) instead
 * @returns {Promise<{code: string, map: object|null, functions: number, skipped: {name: string|null, where: string, line: number|null, reason: string}[]}>}
 *   rejects on syntax Babel does not know
 */
export function compileSource(source, file, o = {}) {
  const opts = { hmr: !!o.hmr, sourcemap: !!o.sourcemap, map: !!o.map && !o.sourcemap };
  const key = `${file}\0${opts.hmr}\0${opts.sourcemap}\0${opts.map}`;
  const hash = createHash('sha1').update(source).digest('hex');
  const hit = results.get(key);
  if (hit && hit.hash === hash) return hit.promise;
  const promise = inWorker(source, file, opts);
  results.set(key, { hash, promise });
  promise.catch(() => {
    if (results.get(key)?.promise === promise) results.delete(key);
  });
  return promise;
}

/**
 * esbuild plugin running the compiler on the app's source files.
 * @param {object} o
 * @param {string} o.reactDir  @orktes/jsui-react's directory (not compiled)
 * @param {boolean} [o.sourcemap]
 * @param {boolean} [o.hmr]  compile for hot updates (jsui dev with HMR: hmr.js
 *   then reuses the result)
 * @param {object} [o.report]  filled with { files, functions, skipped } on every build
 * @param {(msg: string) => void} [o.log]
 */
export function reactCompilerPlugin(o) {
  const isApp = appSourceFilter(o.reactDir);
  return {
    name: 'jsui-react-compiler',
    setup(build) {
      build.onStart(() => {
        if (o.report) Object.assign(o.report, { files: 0, functions: 0, skipped: [] });
      });
      build.onLoad({ filter: SOURCE_EXT, namespace: 'file' }, async (args) => {
        if (!isApp(args.path)) return undefined;
        const source = await readFile(args.path, 'utf8');
        let hit;
        try {
          // With hmr, hmr.js compiles the same files and asks for their maps
          // (the same options share one compile).
          hit = await compileSource(source, args.path, { hmr: o.hmr, sourcemap: o.sourcemap && !o.hmr, map: o.hmr && o.sourcemap });
        } catch (e) {
          // Syntax Babel does not know: esbuild loads the file as written
          // (and reports real syntax errors itself).
          o.log?.(`[jsui] React Compiler skipped ${path.relative(process.cwd(), args.path)}: ${e.message.split('\n')[0]}`);
          hit = { code: null, functions: 0, skipped: [] };
        }
        if (o.report) {
          o.report.files++;
          o.report.functions += hit.functions;
          o.report.skipped.push(...hit.skipped);
        }
        if (hit.code == null) return undefined;
        const ext = path.extname(args.path).slice(1).toLowerCase();
        return { contents: hit.code, loader: ext === 'ts' || ext === 'tsx' ? ext : 'jsx' };
      });
    },
  };
}
