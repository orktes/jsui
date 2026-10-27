// Chrome's DevTools frontend, served by `jsui dev` from its own origin
// (/__jsui/devtools/). Opened from there, DevTools may fetch from this
// server (its content security policy allows 'self'), which devtools:// and
// the hosted copy can't: so source maps load the plain way, and the
// Performance panel opens the multi-track trace of a recording (see
// jsui-devtools.js below) instead of the bare CPU profile.
//
// The files are Chrome's prebuilt frontend (BSD-3-Clause) for one pinned
// Chrome revision, fetched from Google's DevTools CDN on first use and kept
// in a cache directory. The pin matters: the patch hooks the Performance
// panel's internals, so a new revision is a deliberate bump.

import { mkdir, readFile, writeFile, rename } from 'node:fs/promises';
import { homedir } from 'node:os';
import path from 'node:path';

// Chrome 156.0.8078.48 (/json/version's WebKit-Version).
export const DEVTOOLS_REVISION = 'd0c02bfad08ec92a6ffcf989e53cd995e6381a4c';
const CDN = `https://chrome-devtools-frontend.appspot.com/serve_rev/@${DEVTOOLS_REVISION}/`;
const PREFIX = '/__jsui/devtools/';
const ENTRIES = new Set(['js_app.html', 'devtools_app.html', 'inspector.html', 'trace_app.html']);

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.avif': 'image/avif',
  '.woff2': 'font/woff2',
  '.wasm': 'application/wasm',
};

// Loaded by every entry page: the Performance panel opens a recording's
// trace (the relay names it in profile.jsuiTrace) rather than the profile.
const PATCH = `// jsui dev: open a recording as its trace (render thread, worklets, GPU).
import * as Timeline from './panels/timeline/timeline.js';
const proto = Timeline.TimelinePanel.TimelinePanel.prototype;
const loadFromCpuProfile = proto.loadFromCpuProfile;
proto.loadFromCpuProfile = function (profile, title) {
  const url = profile && profile.jsuiTrace;
  if (!url) return loadFromCpuProfile.call(this, profile, title);
  fetch(url)
    .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
    .then((text) => this.loadFromFile(new File([text], 'jsui-profile.json', { type: 'application/json' })))
    .catch(() => loadFromCpuProfile.call(this, profile, title));
};
`;

/** The cache directory for one revision's files. */
export function devtoolsCacheDir(env = process.env) {
  const base = env.JSUI_CACHE_DIR || (env.XDG_CACHE_HOME ? path.join(env.XDG_CACHE_HOME, 'jsui') : path.join(homedir(), '.cache', 'jsui'));
  return path.join(base, 'devtools', DEVTOOLS_REVISION);
}

/** An entry page with the patch loaded and `wsOrigin` allowed to connect. */
export function editEntryPage(html, wsOrigin) {
  let out = html.replace(/connect-src ([^;"]*)/, (m, list) => `connect-src ${list} ${wsOrigin}`);
  out = out.replace(/(<script type="module" src="[^"]+"><\/script>)/, '$1\n<script type="module" src="./jsui-devtools.js"></script>');
  return out;
}

/**
 * @param {{cacheDir?: string, fetch?: typeof fetch, log?: (m: string) => void}} [o]
 * @returns {{handle(req, res, url: URL): boolean, url(host: string, runtimeId: string): string}}
 */
export function createDevtoolsFrontend(o = {}) {
  const dir = o.cacheDir || devtoolsCacheDir();
  const get = o.fetch || fetch;
  const pending = new Map();  // file -> Promise<Buffer>

  async function file(rel) {
    const local = path.join(dir, rel);
    try {
      return await readFile(local);
    } catch {}
    if (!pending.has(rel)) {
      pending.set(rel, (async () => {
        const r = await get(CDN + rel);
        if (!r.ok) throw Object.assign(new Error(`${CDN + rel}: ${r.status}`), { status: r.status });
        const data = Buffer.from(await r.arrayBuffer());
        await mkdir(path.dirname(local), { recursive: true });
        const tmp = `${local}.${process.pid}.tmp`;
        await writeFile(tmp, data);
        await rename(tmp, local);
        return data;
      })().finally(() => pending.delete(rel)));
    }
    return pending.get(rel);
  }

  return {
    url: (host, runtimeId) => `http://${host}${PREFIX}js_app.html?experiments=true&v8only=true&ws=${host}/__jsui/inspect/ws/${encodeURIComponent(runtimeId)}`,
    handle(req, res, url) {
      if (!url.pathname.startsWith(PREFIX)) return false;
      const rel = decodeURIComponent(url.pathname.slice(PREFIX.length));
      if (!rel || rel.split('/').some((p) => p === '..' || p === '') || !/^[\w./@-]+$/.test(rel)) {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('not found');
        return true;
      }
      const send = (data) => {
        res.writeHead(200, { 'Content-Type': TYPES[path.extname(rel)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
        res.end(req.method === 'HEAD' ? undefined : data);
      };
      if (rel === 'jsui-devtools.js') {
        send(PATCH);
        return true;
      }
      file(rel).then(
        (data) => send(ENTRIES.has(rel) ? editEntryPage(data.toString('utf8'), `ws://${req.headers.host}`) : data),
        (e) => {
          res.writeHead(e.status === 404 ? 404 : 502, { 'Content-Type': 'text/plain; charset=utf-8' });
          res.end(`DevTools frontend (revision ${DEVTOOLS_REVISION}) unavailable: ${e.message}\nIt is fetched from ${CDN} on first use; chrome://inspect or the devtools:// link works offline.`);
        },
      );
      return true;
    },
  };
}
