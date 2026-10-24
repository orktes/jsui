// `jsui dev <entry> [--port 5173] [--host 127.0.0.1] [--no-hmr] [--no-inspect] [--react-compiler]`
//
// Rebuilds on change and serves (contract with the runtime's dev mode):
//   GET /bundle.js            last good bundle (build errors keep it served).
//                             With HMR (the default) it is the module-per-
//                             function dev bundle of hmr.js.
//   GET /bundle.js.map        its source map (sources are file:// URLs of
//                             the files on disk, with sourcesContent)
//   GET /assets/*             emitted images / fonts
//   GET /__jsui/version       plain-text opaque token, changes on every
//                             successful rebuild. `?since=<token>` long-polls:
//                             the reply is held (up to ~25 s) until the token
//                             differs from <token>. With `hmr=1` (and HMR on)
//                             a failed build changes the token too, so the
//                             runtime asks /__jsui/hmr and shows the error.
//   GET /__jsui/hmr?from=<v>  hot update script from version <v> to the
//                             current one (see hmr.js / hmr-runtime.js)
//   GET /__jsui/error         text of the current build error ('' when ok)
//   GET /                     small index page (links the WASM web host if built)
//   /shared, /__jsui/shared/*, /__jsui/session/*
//                             the shared session (shared.js): every runtime
//                             loading /bundle.js in dev mode (native viewer,
//                             Godot, the web host, ...) joins it, and agents
//                             (jsui run --attach) drive the live one. With
//                             `headless`, one more runtime runs in this process.
//   /web/*                    the WASM web host (the repo's web/ or the
//                             installed @orktes/jsui-web)
//   /json, /json/list, /json/version, /__jsui/inspect/*
//                             the JS debugger relay (inspect.js; off with
//                             inspect: false): DevTools / VS Code attach to
//                             the runtimes' CDP agents through it.
// Anything else is served from the entry's directory (for fetch('./data.json')).

import * as esbuild from 'esbuild';
import http from 'node:http';
import { existsSync, statSync, createReadStream } from 'node:fs';
import path from 'node:path';
import { esbuildOptions } from './config.js';
import { formatErrors } from './build.js';
import { createHmr } from './hmr.js';
import { REACT_COMPILER_DEFAULT } from './react-compiler.js';
import { createShared } from './shared.js';
import { createInspector } from './inspect.js';
import { prepareMap, concatMaps } from './sourcemap.js';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const CLI_VERSION = require('../package.json').version;

const TYPES = {
  '.js': 'text/javascript; charset=utf-8',
  '.map': 'application/json; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.bmp': 'image/bmp',
  '.svg': 'image/svg+xml',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.wasm': 'application/wasm',
  '.txt': 'text/plain; charset=utf-8',
};

function webPackageDir() {
  try {
    const dir = path.dirname(require.resolve('@orktes/jsui-web/package.json'));
    return existsSync(path.join(dir, 'index.html')) ? dir : null;
  } catch {
    return null;
  }
}

function findWebHost(start) {
  let dir = path.resolve(start);
  for (;;) {
    const candidate = path.join(dir, 'web');
    if (existsSync(path.join(candidate, 'index.html'))) return candidate;
    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

/** Resolves `rel` inside `root`, refusing traversal outside it. */
function safeJoin(root, rel) {
  const p = path.resolve(root, '.' + path.posix.normalize('/' + rel));
  return p === root || p.startsWith(root + path.sep) ? p : null;
}

/**
 * `headless`: null, or the options of the in-process runtime ({width, height,
 * scale, fps, component, props, state, js}). `waitMs`: how long agent commands
 * wait for a runtime to join the session.
 */
export async function dev({ entry, port = 5173, host = '127.0.0.1', log = console.log, minify = false, hmr = true, inspect = true, reactCompiler = REACT_COMPILER_DEFAULT, headless = null, waitMs }) {
  const absEntry = path.resolve(entry);
  const entryDir = path.dirname(absEntry);
  const webDir = findWebHost(entryDir) || findWebHost(process.cwd()) || webPackageDir();
  const state = { bundle: null, map: null, assets: new Map(), version: '0', error: '', builds: 0, failures: 0, started: 0, ended: 0 };
  const hot = hmr ? createHmr({ entry: absEntry, mode: 'development', assets: state.assets, reactCompiler, sourcemap: true, log }) : null;
  // The token runtimes poll with hmr=1: a failed build changes it too.
  const hotToken = () => (hot && state.error ? `${state.version}!${state.failures}` : state.version);
  const waiters = new Set();
  const outfile = path.join(entryDir, '.jsui-dev', 'bundle.js');

  const notify = () => {
    for (const w of waiters) w();
    waiters.clear();
  };

  const reporter = {
    name: 'jsui-dev-reporter',
    setup(b) {
      let t0 = 0;
      b.onStart(() => {
        t0 = Date.now();
        state.started++;
      });
      b.onEnd(async (result) => {
        try {
          await finish(result);
        } finally {
          state.ended++;
        }
      });
      const finish = async (result) => {
        const fail = (text) => {
          state.error = text;
          state.failures++;
          log(`\n[jsui] build failed (serving the last good bundle):\n${state.error}`);
          notify();
        };
        if (result.errors.length) return fail(await formatErrors(result.errors));
        const version = Date.now().toString(36) + '-' + (state.builds + 1);
        let bundle = null;
        let map = null;
        for (const f of result.outputFiles || []) {
          // esbuild's sources are relative to the outfile (.jsui-dev/), which
          // the map's URL (/bundle.js.map) is not: make them file:// URLs.
          if (f.path.endsWith('.map')) map = hot ? null : Buffer.from(JSON.stringify(concatMaps([{ line: 0, map: prepareMap(f.text, path.dirname(outfile)) }], 'bundle.js')));
          else if (f.path.endsWith('.js')) bundle = Buffer.from(f.contents);
        }
        if (hot) {
          try {
            bundle = Buffer.from(await hot.commit(result.metafile, version));
            map = null;
          } catch (e) {
            return fail(e.errors ? await formatErrors(e.errors) : String(e.stack || e));
          }
        }
        state.bundle = bundle;
        state.map = map;
        state.error = '';
        state.builds++;
        state.version = version;
        log(`[jsui] built ${path.relative(process.cwd(), absEntry)} in ${Date.now() - t0} ms (${(state.bundle.length / 1024).toFixed(0)} KiB${hot ? ', hot' : ''}) -> version ${state.version}`);
        notify();
      };
    },
  };

  const opts = esbuildOptions({ entry: absEntry, outfile, mode: 'development', minify, sourcemap: true, assets: state.assets, reactCompiler, compilerHmr: hmr, log });
  opts.plugins.push(reporter);
  const ctx = await esbuild.context(opts);
  await ctx.rebuild().catch(() => {});  // errors are reported by onEnd
  await ctx.watch();

  // Runtimes join the shared session only when the server offers one, and
  // their debugger agent connects only when it sees X-Jsui-Inspect.
  const offer = inspect
    ? { 'X-Jsui-Session': '1', 'X-Jsui-Inspect': '1', 'Access-Control-Expose-Headers': 'X-Jsui-Version, X-Jsui-Session, X-Jsui-Inspect' }
    : { 'X-Jsui-Session': '1', 'Access-Control-Expose-Headers': 'X-Jsui-Version, X-Jsui-Session' };
  const send = (res, status, type, body, extra) => {
    res.writeHead(status, Object.assign({ 'Content-Type': type, 'Cache-Control': 'no-store', 'Access-Control-Allow-Origin': '*' }, extra));
    res.end(body);
  };

  let sharedView = null;
  let origin = `http://127.0.0.1:${port}`;  // set once listening
  // Before listening: a runtime that finds no relay (404) stops trying.
  const inspector = inspect ? createInspector({ origin: () => origin, hosts: [host], version: CLI_VERSION, log }) : null;
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    if (sharedView && sharedView.handle(req, res, url)) return undefined;
    if (inspector && inspector.handle(req, res, url)) return undefined;
    const p = decodeURIComponent(url.pathname);
    // With --no-inspect (inspector null) the debugger's routes are unknown.
    if (p.startsWith('/__jsui/inspect/')) return send(res, 404, 'text/plain', 'not found (jsui dev --no-inspect)');
    if (req.method === 'OPTIONS') return send(res, 204, 'text/plain', '', { 'Access-Control-Allow-Methods': 'GET', 'Access-Control-Allow-Headers': '*' });
    if (req.method !== 'GET' && req.method !== 'HEAD') return send(res, 405, 'text/plain', 'method not allowed');
    if (p === '/bundle.js') {
      if (!state.bundle) return send(res, 503, 'text/plain; charset=utf-8', 'no successful build yet\n' + state.error);
      // The runtime compares this with /__jsui/version to detect changes
      // that landed between loading the bundle and its first poll.
      return send(res, 200, TYPES['.js'], state.bundle, { 'X-Jsui-Version': state.version, ...offer });
    }
    if (p === '/bundle.js.map') {
      const map = hot ? hot.sourceMap() : state.map;
      return map ? send(res, 200, TYPES['.map'], map) : send(res, 404, 'text/plain', 'no map');
    }
    if (p === '/__jsui/version') {
      const token = url.searchParams.get('hmr') === '1' ? hotToken : () => state.version;
      const since = url.searchParams.get('since');
      if (since == null || since !== token()) return send(res, 200, 'text/plain; charset=utf-8', token(), offer);
      let done = false;
      const finish = () => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        waiters.delete(finish);
        send(res, 200, 'text/plain; charset=utf-8', token(), offer);
      };
      const timer = setTimeout(finish, 25000);
      waiters.add(finish);
      req.on('close', () => {
        done = true;
        clearTimeout(timer);
        waiters.delete(finish);
      });
      return undefined;
    }
    if (p === '/__jsui/hmr') {
      if (!hot) return send(res, 200, TYPES['.js'], '__jsui_hmr.reload("hot updates are off (jsui dev --no-hmr)");\n');
      // The runtime names the update script after the version (DevTools lists it).
      return send(res, 200, TYPES['.js'], hot.update(url.searchParams.get('from') || '', state.error), { 'X-Jsui-Version': state.version, 'Access-Control-Expose-Headers': 'X-Jsui-Version' });
    }
    if (p === '/__jsui/error') return send(res, 200, 'text/plain; charset=utf-8', state.error);
    if (p.startsWith('/assets/')) {
      const buf = state.assets.get(p.slice(1));
      if (buf) return send(res, 200, TYPES[path.extname(p).toLowerCase()] || 'application/octet-stream', buf);
    }
    if (p === '/' || p === '/index.html') {
      const web = webDir ? `<p><a href="/web/index.html?bundle=/bundle.js">Open in the JSUI web (WASM) host</a></p>` : `<p>No WASM web host found (build <code>web/</code> to preview in the browser).</p>`;
      const html = `<!doctype html><meta charset="utf-8"><title>jsui dev</title>
<style>body{font:14px/1.5 system-ui,sans-serif;margin:2rem;color:#ddd;background:#111}a{color:#7cf}code{color:#fc6}</style>
<h1>jsui dev server</h1>
<p>Bundle: <a href="/bundle.js">/bundle.js</a> &middot; version: <code>${state.version}</code></p>
<p>Native: <code>jsui-viewer --dev http://${host}:${port}/bundle.js</code></p>
<p><a href="/shared">Shared session</a> (agents: <code>jsui run --attach http://${host}:${port} steps.json</code>)</p>
${web}
${state.error ? `<pre style="color:#f88">${state.error.replace(/[<&]/g, (c) => (c === '<' ? '&lt;' : '&amp;'))}</pre>` : ''}`;
      return send(res, 200, TYPES['.html'], html);
    }
    let file = null;
    if (webDir && p.startsWith('/web/')) file = safeJoin(webDir, p.slice(5));
    else file = safeJoin(entryDir, p.slice(1));
    if (file && existsSync(file) && statSync(file).isFile()) {
      res.writeHead(200, { 'Content-Type': TYPES[path.extname(file).toLowerCase()] || 'application/octet-stream', 'Cache-Control': 'no-store', 'Access-Control-Allow-Origin': '*' });
      createReadStream(file).pipe(res);
      return undefined;
    }
    return send(res, 404, 'text/plain', 'not found');
  });
  server.on('upgrade', (req, socket, head) => {
    if (inspector && inspector.upgrade(req, socket, head)) return;
    socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  const actualPort = server.address().port;
  log(`[jsui] dev server: http://${host}:${actualPort}/bundle.js  (version endpoint /__jsui/version)`);
  // The runtimes fetch the bundle (and hot updates) from this server.
  const local = host === '0.0.0.0' || host === '::' ? '127.0.0.1' : host.includes(':') ? `[${host}]` : host;
  origin = `http://${local}:${actualPort}`;
  try {
    sharedView = await createShared({
      ...(headless || {}),
      headless: !!headless,
      waitMs,
      entry: path.relative(process.cwd(), absEntry),
      bundleUrl: `${origin}/bundle.js`,
      origin,
      webHost: webDir ? '/web/index.html' : null,
      hosts: [host, local],
      log,
    });
  } catch (e) {
    await ctx.dispose();
    if (inspector) inspector.close();
    await new Promise((r) => server.close(r));
    throw e;
  }
  log(`[jsui] shared session: ${origin}/shared  (agents: jsui run --attach ${origin} steps.json)`);
  if (inspect) {
    const at = `${local}:${actualPort}`;
    // resolveSourceMapLocations: null lets js-debug use maps served over
    // http (its default only allows maps under the workspace folder).
    const address = local.replace(/^\[|\]$/g, '');
    log(`[jsui] debugger: chrome://inspect -> Configure... -> add ${at}; VS Code launch.json: {"type":"node","request":"attach","name":"JSUI","address":"${address}","port":${actualPort},"resolveSourceMapLocations":null}  (off: --no-inspect)`);
  }
  return {
    port: actualPort,
    state,
    shared: sharedView,
    get inspector() {
      return inspector;
    },
    async close() {
      notify();
      if (inspector) inspector.close();
      if (sharedView) await sharedView.close();
      await ctx.dispose();
      await new Promise((r) => server.close(r));
      server.closeAllConnections?.();
    },
  };
}
