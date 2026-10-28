// JS debugging for `jsui dev`: a relay between DevTools frontends (Chrome
// DevTools via chrome://inspect, VS Code's js-debug) and the JSUI runtimes
// that load the dev bundle.
//
// The runtime's CDP agent (quickjs-cdp.c) has no sockets of its own: it
// tunnels CDP over HTTP long-polls on the dev server's port, from its own
// threads, so it keeps answering while JS is paused at a breakpoint. The
// server only relays; the runtime implements the protocol.
//
//   runtime -> server (POST, JSON bodies)
//     /__jsui/inspect/poll  {runtime, name, title}: registers the runtime as a
//                           target; held (up to 20 s) until there are events
//                           for it: {"events": [{type: open|close|message,
//                           session, message?}]}
//     /__jsui/inspect/push  {runtime, messages: [{session|null, message}]}:
//                           CDP text to that frontend (null: all of them)
//     /__jsui/inspect/bye   {runtime}: closes its frontends, drops the target
//   frontends -> server
//     GET /json/version, /json, /json/list   the DevTools HTTP discovery API
//     WebSocket /__jsui/inspect/ws/<runtime id>  one CDP session per connection
//
// A runtime that has not polled for 30 s is gone (its frontends are closed).
// WebSocket (RFC 6455) is implemented here: no dependency, text frames out.

import { createHash } from 'node:crypto';
import { profileTrace } from './profile-trace.js';

const POLL_HOLD_MS = 20000;
const GONE_AFTER_MS = 30000;         // no poll for this long: the runtime left
const DROPPED_GONE_MS = 3000;        // ... or this long after it dropped a held poll
const WAIT_FOR_RUNTIME_MS = 30000;   // a frontend waits this long for an unknown runtime
const REGREET_AFTER_MS = 5000;       // a poll after this gap re-sends `open` for open sessions
const MAX_QUEUE = 10000;             // events waiting for a runtime's poll
const MAX_BODY = 256 << 20;          // CDP replies can carry whole scripts
const MAX_FRAME = 256 << 20;
const ID = /^[\w.-]{1,128}$/;
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

// ------------------------------------------------------------------ WebSocket

/**
 * Server side of one WebSocket connection (RFC 6455): reads masked client
 * frames (7/16/64-bit lengths, fragmented messages, ping/pong, close) and
 * sends unfragmented, unmasked text frames.
 */
class WsConnection {
  constructor(socket, head, { onMessage, onClose }) {
    this.socket = socket;
    this.onMessage = onMessage;
    this.onClose = onClose;
    this.buf = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
    this.fragments = null;  // chunks of a fragmented message
    this.fragmentsLength = 0;
    this.closed = false;
    this.closeSent = false;
    socket.setNoDelay(true);
    socket.on('data', (d) => {
      this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
      this._parse();
    });
    socket.on('close', () => this._finish());
    socket.on('error', () => this._finish());
    if (this.buf.length) queueMicrotask(() => this._parse());
  }

  _parse() {
    while (!this.closed) {
      const b = this.buf;
      if (b.length < 2) return;
      const fin = (b[0] & 0x80) !== 0;
      const rsv = b[0] & 0x70;
      const opcode = b[0] & 0x0f;
      const masked = (b[1] & 0x80) !== 0;
      let len = b[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (b.length < 4) return;
        len = b.readUInt16BE(2);
        off = 4;
      } else if (len === 127) {
        if (b.length < 10) return;
        const hi = b.readUInt32BE(2);
        if (hi > 0x1fffff) return this.close(1009, 'frame too large');
        len = hi * 2 ** 32 + b.readUInt32BE(6);
        off = 10;
      }
      if (rsv) return this.close(1002, 'unexpected RSV bits');
      if (!masked) return this.close(1002, 'client frames must be masked');
      if (len > MAX_FRAME) return this.close(1009, 'frame too large');
      if (b.length < off + 4 + len) return;
      const mask = b.subarray(off, off + 4);
      const payload = Buffer.from(b.subarray(off + 4, off + 4 + len));
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      this.buf = b.subarray(off + 4 + len);
      if (opcode >= 8) {
        // Control frames: never fragmented, at most 125 bytes.
        if (!fin || len > 125) return this.close(1002, 'bad control frame');
        if (opcode === 8) {
          const code = payload.length >= 2 ? payload.readUInt16BE(0) : 1005;
          this.close(code === 1005 || code === 1006 || code === 1015 ? 1000 : code);
          return undefined;
        }
        if (opcode === 9) this._frame(10, payload);  // ping -> pong
        else if (opcode !== 10) return this.close(1002, 'unknown opcode');
        continue;
      }
      if (opcode === 0) {
        if (!this.fragments) return this.close(1002, 'unexpected continuation frame');
        this.fragments.push(payload);
        this.fragmentsLength += payload.length;
      } else if (opcode === 1 || opcode === 2) {
        if (this.fragments) return this.close(1002, 'expected a continuation frame');
        this.fragments = [payload];
        this.fragmentsLength = payload.length;
      } else {
        return this.close(1002, 'unknown opcode');
      }
      if (this.fragmentsLength > MAX_FRAME) return this.close(1009, 'message too large');
      if (fin) {
        const data = this.fragments.length === 1 ? this.fragments[0] : Buffer.concat(this.fragments);
        this.fragments = null;
        this.fragmentsLength = 0;
        this.onMessage(data.toString('utf8'));
      }
    }
    return undefined;
  }

  _frame(opcode, payload) {
    if (this.socket.destroyed || this.closeSent) return;
    const len = payload.length;
    let header;
    if (len < 126) {
      header = Buffer.from([0x80 | opcode, len]);
    } else if (len < 65536) {
      header = Buffer.alloc(4);
      header[0] = 0x80 | opcode;
      header[1] = 126;
      header.writeUInt16BE(len, 2);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 127;
      header.writeUInt32BE(Math.floor(len / 2 ** 32), 2);
      header.writeUInt32BE(len >>> 0, 6);
    }
    this.socket.write(Buffer.concat([header, payload]));
  }

  /** Sends a text message. */
  send(text) {
    if (!this.closed) this._frame(1, Buffer.from(text, 'utf8'));
  }

  /** Sends a close frame and ends the connection. */
  close(code = 1000, reason = '') {
    if (this.closed) return;
    const r = Buffer.from(reason, 'utf8').subarray(0, 123);
    const payload = Buffer.alloc(2 + r.length);
    payload.writeUInt16BE(code, 0);
    r.copy(payload, 2);
    this._frame(8, payload);
    this.closeSent = true;
    this._finish();
    this.socket.end();
    // A peer that never answers our close is cut off.
    setTimeout(() => this.socket.destroy(), 1000).unref();
  }

  _finish() {
    if (this.closed) return;
    this.closed = true;
    this.onClose();
  }

  destroy() {
    this._finish();
    this.socket.destroy();
  }
}

/** Answers a WebSocket handshake; null (after answering with an error) when it is not one. */
function acceptWebSocket(req, socket) {
  const key = req.headers['sec-websocket-key'];
  const upgrade = String(req.headers.upgrade || '').toLowerCase();
  if (req.method !== 'GET' || upgrade !== 'websocket' || !key || String(req.headers['sec-websocket-version']) !== '13') {
    socket.end('HTTP/1.1 400 Bad Request\r\nSec-WebSocket-Version: 13\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
    return false;
  }
  const accept = createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  return true;
}

// ------------------------------------------------------------------ relay

/**
 * Creates the relay of a dev server.
 * @param {object} o
 * @param {string|(() => string)} o.origin  the dev server's http origin (for /json/list when the Host header is missing)
 * @param {string[]} [o.hosts]  Host names accepted besides localhost / IP addresses (DNS rebinding guard)
 * @param {string} [o.version]  the CLI version (/json/version)
 * @param {(msg: string) => void} [o.log]
 * @param {{url(host: string, runtimeId: string): string}} [o.devtools]  the DevTools frontend jsui dev serves (devtools-frontend.js)
 */
export function createInspector(o) {
  const log = o.log || console.log;
  const allowedHosts = new Set(['localhost', ...(o.hosts || [])].map((h) => h.toLowerCase()));
  const runtimes = new Map();  // id -> runtime
  const sessions = new Map();  // sid -> session
  let nextSession = 1;
  let closed = false;

  const now = () => Date.now();
  const originOf = () => (typeof o.origin === 'function' ? o.origin() : o.origin);

  function runtimeOf(id) {
    let rt = runtimes.get(id);
    if (!rt) {
      rt = { id, name: '', title: '', live: false, lastSeen: 0, lastPollEnd: 0, poll: null, queue: [], sessions: new Set() };
      runtimes.set(id, rt);
    }
    return rt;
  }

  function enqueue(rt, ev) {
    if (rt.queue.length >= MAX_QUEUE) {
      // A runtime that does not drain its events: start over.
      for (const s of [...rt.sessions]) closeSession(s, 1011, 'too many messages queued for the runtime');
      rt.queue.length = 0;
      return;
    }
    rt.queue.push(ev);
    flush(rt);
  }

  function answerPoll(rt, events) {
    const p = rt.poll;
    if (!p) return;
    rt.poll = null;
    clearTimeout(p.timer);
    rt.lastPollEnd = now();
    rt.lastSeen = now();
    sendJson(p.res, 200, { events });
  }

  function flush(rt) {
    if (rt.poll && rt.queue.length) answerPoll(rt, rt.queue.splice(0));
  }

  function closeSession(s, code = 1000, reason = '') {
    if (!sessions.has(s.id)) return;
    sessions.delete(s.id);
    clearTimeout(s.waitTimer);
    const rt = runtimes.get(s.runtime);
    if (rt) {
      rt.sessions.delete(s);
      if (rt.live) enqueue(rt, { type: 'close', session: s.id });
      else rt.queue = rt.queue.filter((e) => e.session !== s.id);  // never delivered: forget them
      if (!rt.live && !rt.sessions.size) runtimes.delete(rt.id);
    }
    s.ws.close(code, reason);
    log(`[jsui] debugger: frontend ${s.id} detached from ${rt ? label(rt) : s.runtime}`);
  }

  function dropRuntime(rt, why) {
    runtimes.delete(rt.id);
    if (rt.poll) answerPoll(rt, []);
    rt.live = false;
    for (const s of [...rt.sessions]) closeSession(s, 1001, why);
    if (why) log(`[jsui] debugger: runtime ${label(rt)} ${why}`);
  }

  const label = (rt) => (rt.name ? `'${rt.name}'` : rt.id);

  // Runtimes that stopped polling, frontends that waited too long.
  const sweeper = setInterval(() => {
    const t = now();
    for (const rt of [...runtimes.values()]) {
      if (rt.live && !rt.poll && t - rt.lastSeen > GONE_AFTER_MS) dropRuntime(rt, 'left (no poll for 30 s)');
      else if (rt.live && !rt.poll && rt.droppedAt && t - rt.droppedAt > DROPPED_GONE_MS) dropRuntime(rt, 'left (its connection closed)');
    }
  }, 1000);
  sweeper.unref();

  // ---------------------------------------------------------- HTTP helpers

  function sendJson(res, status, obj) {
    if (res.writableEnded || res.destroyed) return;
    const body = JSON.stringify(obj);
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store', 'Content-Length': Buffer.byteLength(body) });
    res.end(body);
  }

  const readJson = (req) =>
    new Promise((resolve, reject) => {
      const chunks = [];
      let n = 0;
      req.on('data', (c) => {
        n += c.length;
        if (n > MAX_BODY) {
          reject(new Error('body too large'));
          req.destroy();
        } else chunks.push(c);
      });
      req.on('end', () => {
        try {
          resolve(JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}'));
        } catch (e) {
          reject(new Error('invalid JSON: ' + e.message));
        }
      });
      req.on('error', reject);
    });

  // DNS rebinding guard: a page of another site resolving its own name to
  // 127.0.0.1 must not reach the debugger (it could run code in the app).
  const hostOk = (req) => {
    const h = String(req.headers.host || '').toLowerCase().replace(/:\d+$/, '');
    if (!h) return true;
    return allowedHosts.has(h) || /^\d+\.\d+\.\d+\.\d+$/.test(h) || /^\[[\da-f:.]+\]$/.test(h);
  };
  // Web pages can open WebSockets anywhere: only DevTools (devtools://) and
  // clients without an Origin (VS Code, Node) may attach.
  const originOk = (req) => {
    const origin = req.headers.origin;
    if (origin == null) return true;
    if (/^(devtools|chrome-devtools):\/\//i.test(origin)) return true;
    try {
      const u = new URL(origin);
      return u.host === String(req.headers.host || '') && hostOk(req);
    } catch {
      return false;
    }
  };

  const hostOf = (req) => (req.headers.host && hostOk(req) ? String(req.headers.host) : new URL(originOf()).host);

  function target(rt, host) {
    const ws = `${host}/__jsui/inspect/ws/${rt.id}`;
    return {
      description: 'JSUI runtime',
      devtoolsFrontendUrl: `devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=${ws}`,
      devtoolsFrontendUrlCompat: `devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=${ws}`,
      // DevTools as for a page: the Performance panel records a trace (Tracing), with a track per thread.
      jsuiPageFrontendUrl: `devtools://devtools/bundled/inspector.html?ws=${host}/__jsui/inspect/page/${rt.id}`,
      faviconUrl: '',
      id: rt.id,
      title: rt.name && rt.title ? `${rt.name} — ${rt.title}` : rt.name || rt.title || rt.id,
      type: 'node',
      url: rt.title || '',
      webSocketDebuggerUrl: `ws://${ws}`,
    };
  }

  // ---------------------------------------------------------- runtimes' side

  async function runtimePost(req, res, p) {
    if (!/^application\/json\b/i.test(String(req.headers['content-type'] || ''))) return sendJson(res, 415, { error: 'Content-Type must be application/json' });
    let body;
    try {
      body = await readJson(req);
    } catch (e) {
      return sendJson(res, 400, { error: e.message });
    }
    const id = String((body && body.runtime) || '');
    if (!ID.test(id)) return sendJson(res, 400, { error: 'expected {"runtime": "<id>"}' });
    if (p === '/__jsui/inspect/poll') {
      if (closed) return sendJson(res, 200, { events: [] });
      const rt = runtimeOf(id);
      const t = now();
      if (typeof body.name === 'string') rt.name = body.name.slice(0, 200);
      if (typeof body.title === 'string') rt.title = body.title.slice(0, 2000);
      if (typeof body.profiler === 'boolean') rt.profiler = body.profiler;
      if (typeof body.network === 'boolean') rt.network = body.network;
      if (typeof body.dom === 'boolean') rt.dom = body.dom;
      const gap = !rt.poll && t - rt.lastPollEnd > REGREET_AFTER_MS;
      if (rt.poll) answerPoll(rt, []);  // only one poll per runtime
      if (!rt.live) {
        rt.live = true;
        log(`[jsui] debugger: runtime ${label(rt)} available (ws://${hostOf(req)}/__jsui/inspect/ws/${rt.id})` +
          (o.devtools ? `\n[jsui] DevTools: http://${hostOf(req)}/__jsui/devtools/ (the live runtime), or Chrome's own: devtools://devtools/bundled/inspector.html?ws=${hostOf(req)}/__jsui/inspect/page/${rt.id}` : ''));
        for (const s of rt.sessions) clearTimeout(s.waitTimer);
      } else if (gap) {
        // A runtime that comes back (restarted its agent) learns the
        // sessions that are still open.
        const pending = new Set(rt.queue.filter((e) => e.type === 'open').map((e) => e.session));
        const opens = [...rt.sessions].filter((s) => !pending.has(s.id)).map((s) => ({ type: 'open', session: s.id }));
        if (opens.length) rt.queue.unshift(...opens);
      }
      rt.lastSeen = t;
      rt.droppedAt = 0;
      const timer = setTimeout(() => {
        if (rt.poll && rt.poll.res === res) answerPoll(rt, []);
      }, POLL_HOLD_MS);
      rt.poll = { res, timer };
      res.on('close', () => {
        if (rt.poll && rt.poll.res === res) {
          // Dropped mid-poll: the events stay queued for the next one. A
          // runtime that does not poll again soon has quit.
          clearTimeout(timer);
          rt.poll = null;
          rt.droppedAt = now();
          rt.lastPollEnd = now();
          rt.lastSeen = now();
        }
      });
      flush(rt);
      return undefined;
    }
    const rt = runtimes.get(id);
    if (p === '/__jsui/inspect/push') {
      if (rt) {
        rt.lastSeen = now();
        for (const m of Array.isArray(body.messages) ? body.messages : []) {
          if (!m || typeof m.message !== 'string') continue;
          if (m.session == null) {
            for (const s of rt.sessions) s.ws.send(m.message);
          } else {
            const s = sessions.get(String(m.session));
            if (s && s.runtime === id && s.page && internalReply(s, rt, m.message)) continue;
            if (s && s.runtime === id) s.ws.send(m.message);
          }
        }
      }
      return sendJson(res, 200, {});
    }
    // bye
    if (rt) dropRuntime(rt, 'closed');
    return sendJson(res, 200, {});
  }

  // ---------------------------------------------------------- frontends' side

  /** Handles the relay's HTTP routes; false for other requests. */
  function handle(req, res, url) {
    const p = url.pathname;
    if (p === '/__jsui/inspect/poll' || p === '/__jsui/inspect/push' || p === '/__jsui/inspect/bye') {
      if (req.method !== 'POST') {
        sendJson(res, 405, { error: 'POST only' });
        return true;
      }
      runtimePost(req, res, p).catch((e) => sendJson(res, 500, { error: String((e && e.message) || e) }));
      return true;
    }
    if (p === '/json' || p === '/json/list' || p === '/json/version' || p === '/json/list/' || p === '/json/version/') {
      if (req.method !== 'GET' && req.method !== 'HEAD') return false;
      if (!hostOk(req)) {
        sendJson(res, 403, { error: 'forbidden host' });
        return true;
      }
      if (p.startsWith('/json/version')) {
        sendJson(res, 200, { Browser: `jsui/${o.version || '0.0.0'}`, 'Protocol-Version': '1.3' });
        return true;
      }
      const host = hostOf(req);
      sendJson(res, 200, [...runtimes.values()].filter((rt) => rt.live).map((rt) => target(rt, host)));
      return true;
    }
    if (o.devtools && (p === '/__jsui/devtools' || p === '/__jsui/devtools/') && (req.method === 'GET' || req.method === 'HEAD')) {
      // DevTools on the live runtime: one link that outlasts restarts.
      const live = [...runtimes.values()].filter((rt) => rt.live).sort((a, b) => b.lastSeen - a.lastSeen)[0];
      if (live) {
        res.writeHead(302, { Location: o.devtools.url(hostOf(req), live.id), 'Cache-Control': 'no-store' });
        res.end();
      } else {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
        res.end('<!doctype html><meta http-equiv="refresh" content="1"><title>DevTools</title><p style="font:14px system-ui">Waiting for a runtime in dev mode to connect…</p>');
      }
      return true;
    }
    if (p.startsWith('/__jsui/inspect/')) {
      sendJson(res, 404, { error: 'not found' });
      return true;
    }
    return false;
  }

  // DevTools loads source maps through the target (Network.loadNetworkResource,
  // then IO.read). The runtime has no use for that detour: the relay answers
  // it, for this server's own URLs only. A read past the end answers eof
  // with no data: DevTools drops the data of a read that says eof.
  const streams = new Map();  // handle -> remaining text
  let nextStream = 1;
  function servesLocally(s, text) {
    if (!text.includes('"Network.loadNetworkResource"') && !text.includes('"IO.')) return false;
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      return false;
    }
    const reply = (result) => s.ws.send(JSON.stringify({ id: msg.id, result }));
    const params = msg.params || {};
    if (msg.method === 'Network.loadNetworkResource') {
      let url = null;
      try {
        url = new URL(params.url);
      } catch {}
      const own = new URL(originOf());
      if (!url || url.protocol !== 'http:' || url.port !== own.port || (!allowedHosts.has(url.hostname.toLowerCase()) && url.hostname !== own.hostname)) {
        reply({ resource: { success: false, netError: -10, netErrorName: 'net::ERR_ACCESS_DENIED' } });
        return true;
      }
      url.hostname = own.hostname;
      fetch(url).then(async (r) => {
        const handle = 'jsui-' + nextStream++;
        streams.set(handle, await r.text());
        reply({ resource: { success: r.ok, httpStatusCode: r.status, stream: handle, headers: {} } });
      }, () => reply({ resource: { success: false, netError: -2, netErrorName: 'net::ERR_FAILED' } }));
      return true;
    }
    if ((msg.method === 'IO.read' || msg.method === 'IO.close') && streams.has(params.handle)) {
      if (msg.method === 'IO.close') {
        streams.delete(params.handle);
        reply({});
      } else {
        const rest = streams.get(params.handle);
        const size = params.size > 0 ? params.size : 1 << 20;
        streams.set(params.handle, rest.slice(size));
        reply({ data: rest.slice(0, size), eof: rest.length === 0, base64Encoded: false });
      }
      return true;
    }
    return false;
  }

  // Runtimes without a profiler acknowledge every method they don't know with an
  // empty result. For these domains that misleads DevTools: it reads fields
  // that aren't there (Network.emulateNetworkConditionsByRule's ruleIds threw
  // while the target was set up), or waits for a profile that never comes
  // (Profiler.stop hung the Performance panel). Refuse them as V8 does, and
  // Profiler too unless the runtime says it records profiles.
  const UNSUPPORTED = /^(?:Network|Profiler|HeapProfiler)\./;
  function refusesUnsupported(s, text) {
    if (!/"method"\s*:\s*"(?:Network|Profiler|HeapProfiler)\./.test(text)) return false;
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      return false;
    }
    if (typeof msg.method !== 'string' || !UNSUPPORTED.test(msg.method)) return false;
    if (msg.method.startsWith('Profiler.') && runtimes.get(s.runtime)?.profiler) return false;
    if (msg.method.startsWith('Network.') && runtimes.get(s.runtime)?.network) return false;
    s.ws.send(JSON.stringify({ id: msg.id, error: { code: -32601, message: `'${msg.method}' wasn't found` } }));
    return true;
  }

  // Page mode (/__jsui/inspect/page/<id>): DevTools opened as for a page
  // (inspector.html) records with Tracing, a track per thread, where a
  // Node-style target only gets a CPU profile. The relay plays the page's
  // part: one frame, its history, and Tracing.start / end, which it turns
  // into a CPU profile of the runtime (Profiler.*, with ids of its own) and
  // answers with the trace profile-trace.js builds from it.
  const INTERNAL = 2000000000;  // message ids the relay sends for itself
  let nextInternal = INTERNAL;
  function pageAnswers(s, text) {
    if (!/"method"\s*:\s*"(Page|Tracing|DOM)\./.test(text)) return false;
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      return false;
    }
    const rt = runtimes.get(s.runtime);
    const reply = (result) => s.ws.send(JSON.stringify({ id: msg.id, result }));
    const url = (rt && rt.title) || originOf() + '/bundle.js';
    switch (msg.method) {
      case 'Page.getResourceTree':
        reply({ frameTree: { frame: { id: 'F1', loaderId: 'L1', url, domainAndRegistry: '', securityOrigin: originOf(), mimeType: 'text/html', secureContextType: 'InsecureScheme', crossOriginIsolatedContextType: 'NotIsolated', gatedAPIFeatures: [] }, resources: [] } });
        return true;
      case 'Page.getNavigationHistory':
        reply({ currentIndex: 0, entries: [{ id: 0, url, userTypedURL: url, title: (rt && rt.name) || 'jsui', transitionType: 'typed' }] });
        return true;
      case 'DOM.getDocument':
        if (rt && rt.dom) return false;  // the runtime's view tree (Elements)
        s.ws.send(JSON.stringify({ id: msg.id, error: { code: -32000, message: 'A JSUI runtime has no DOM' } }));
        return true;
      case 'Tracing.getCategories':
        reply({ categories: ['devtools.timeline', 'disabled-by-default-v8.cpu_profiler'] });
        return true;
      case 'Tracing.start':
        reply({});
        runtimeCall(s, 'Profiler.enable', {});
        runtimeCall(s, 'Profiler.setSamplingInterval', { interval: 100 });
        runtimeCall(s, 'Profiler.start', {});
        return true;
      case 'Tracing.end':
        reply({});
        runtimeCall(s, 'Profiler.stop', {}, (res) => {
          const profile = res && res.result && res.result.profile;
          const events = profile ? profileTrace(profile, { name: (rt && rt.name) || 'jsui', url }).traceEvents : [];
          for (let i = 0; i < events.length; i += 5000) s.ws.send(JSON.stringify({ method: 'Tracing.dataCollected', params: { value: events.slice(i, i + 5000) } }));
          s.ws.send(JSON.stringify({ method: 'Tracing.tracingComplete', params: { dataLossOccurred: !profile } }));
        });
        return true;
      default:
        if (msg.method.startsWith('Page.')) {
          reply(msg.method === 'Page.addScriptToEvaluateOnNewDocument' ? { identifier: '1' } : {});
          return true;
        }
        return false;
    }
  }
  function runtimeCall(s, method, params, done) {
    const r = runtimes.get(s.runtime);
    if (!r) return done && done(null);
    const id = nextInternal++;
    s.page.internal.set(id, done || null);
    enqueue(r, { type: 'message', session: s.id, message: JSON.stringify({ id, method, params }) });
  }
  function internalReply(s, rt, text) {
    const m = /^\{"id":(\d+),/.exec(text);
    if (!m || Number(m[1]) < INTERNAL || !s.page.internal.has(Number(m[1]))) return false;
    const done = s.page.internal.get(Number(m[1]));
    s.page.internal.delete(Number(m[1]));
    if (done) {
      let msg = null;
      try {
        msg = JSON.parse(text);
      } catch {}
      done(msg);
    }
    return true;
  }

  /** Handles an 'upgrade' request of the server; false when it is not the relay's. */
  function upgrade(req, socket, head) {
    const url = new URL(req.url, 'http://x');
    const m = /^\/__jsui\/inspect\/(ws|page)\/([^/]+)$/.exec(url.pathname);
    if (!m) return false;
    const id = decodeURIComponent(m[2]);
    if (closed || !ID.test(id) || !hostOk(req) || !originOk(req)) {
      socket.end(`HTTP/1.1 ${closed || !ID.test(id) ? '404 Not Found' : '403 Forbidden'}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
      return true;
    }
    if (!acceptWebSocket(req, socket)) return true;
    const rt = runtimeOf(id);
    const s = { id: String(nextSession++), runtime: id, ws: null, waitTimer: null, page: m[1] === 'page' ? { internal: new Map() } : null };
    s.ws = new WsConnection(socket, head, {
      onMessage: (text) => {
        if (servesLocally(s, text) || (s.page && pageAnswers(s, text)) || refusesUnsupported(s, text)) return;
        const r = runtimes.get(id);
        if (r && sessions.has(s.id)) enqueue(r, { type: 'message', session: s.id, message: text });
      },
      onClose: () => closeSession(s, 1000),
    });
    sessions.set(s.id, s);
    rt.sessions.add(s);
    enqueue(rt, { type: 'open', session: s.id });
    if (!rt.live) {
      // Queued until the runtime polls (it may be starting or reloading).
      s.waitTimer = setTimeout(() => {
        if (!rt.live) closeSession(s, 1011, `no JSUI runtime ${id} connected`);
      }, WAIT_FOR_RUNTIME_MS);
      s.waitTimer.unref();
      log(`[jsui] debugger: frontend ${s.id} waiting for runtime ${id}`);
    } else {
      log(`[jsui] debugger: frontend ${s.id} attached to ${label(rt)}`);
    }
    return true;
  }

  return {
    handle,
    upgrade,
    /** Live targets (as in /json/list). */
    targets: () => [...runtimes.values()].filter((rt) => rt.live).map((rt) => target(rt, new URL(originOf()).host)),
    close() {
      closed = true;
      clearInterval(sweeper);
      for (const rt of [...runtimes.values()]) dropRuntime(rt, '');
      for (const s of [...sessions.values()]) closeSession(s, 1001, 'dev server stopped');
    },
  };
}
