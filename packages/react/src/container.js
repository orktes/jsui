// JSUIContainer: runs another JSUI bundle inside this one, as an isolated
// runtime of its own (its own JS context, React tree, input focus, fonts and
// images) shown in this view. "JSUI inside JSUI": apps that host apps built
// with JSUI (src/render/container_view.h).
//
//   <JSUIContainer
//     bundleSrc="apps/solitaire.js"        // through the host's resource loader, like images
//     // bundleData={source}               // or the bundle source itself
//     appKey="Solitaire" initialProps={{ seed: 7 }}
//     hostState={{ theme: 'dark' }}        // the child's HostState / useHostState keys
//     onMessage={(name, payload) => ...}   // the child's Host.postMessage
//     onCall={async (name, args) => ...}   // the child's Host.call (return the answer)
//     onStateChange={(key, value) => ...}  // the child's HostState.set / patch
//     ref={ref}                            // ref.current.postMessage / call / setState / patchState / reload
//     services={{ load: { deny: ['https://ads.*'] }, audio: 'none' }}  // see below
//     style={{ flex: 1 }}
//   />
//
// Services. The child uses this runtime's services unless `services` says
// otherwise: its loads (bundle, images, fetch) go through this runtime's
// loader, its sounds and videos through this runtime's audio / video (the
// host's handlers), and it sees this runtime's fonts and host textures
// under its own. Per service:
//
//   load:  'inherit' (default) | 'none' | (req, next) => ... |
//          { allow, deny: [glob], rewrite: {fromPrefix: toPrefix}, handle: (req, next) => ... }
//          allow / deny / rewrite are matched natively (cheap, for images);
//          `*` in a glob matches anything. A refused load fails like a
//          network error.
//   audio: 'inherit' | 'none' | (cmd, next) => ...
//   video: 'inherit' | 'none' | (cmd, next) => ...
//   fonts: 'inherit' | 'none'
//   textures: 'inherit' | 'none' | { inherit: false, alias: {childKey: parentKey} }
//
// Middleware (req / cmd, next): call next() to pass the request on (next(changed)
// to pass on a changed copy), return null / false to refuse it, return
// nothing to pass it on unchanged. A load middleware may also answer
// itself: return a string (the body) or {status, statusText, headers,
// body}. May be async. req: {url, method, headers, body}; cmd: the child's
// audio / video command ({op: 'play', sound, bus, volume, ...}, {op: 'open',
// sources: [{uri, type}], ...}).
//
// The container is the child's embedder: everything a game does with a JSUI
// runtime through the C API, the parent does here. Its content box is the
// child's viewport; pointer input over it goes to the child, and keys and
// gamepad input while it has focus (what the child does not use goes on in
// the parent). Hosts without native containers (react-native-web, Node
// tests) get an empty View of the same style.

import { useImperativeHandle, useLayoutEffect, useRef } from 'react';
import { jsx } from 'react/jsx-runtime';
import { native, warnOnce } from './native.js';

const hasNative = typeof native.containerCommand === 'function';

const OMIT = {
  bundleData: 1, hostState: 1, initialProps: 1, ref: 1, services: 1,
  onMessage: 1, onCall: 1, onCallCancel: 1, onStateChange: 1, onError: 1,
};

const json = (v) => (v === undefined ? null : v);

const MODES = { inherit: 'inherit', none: 'none' };

// The `services` prop -> what native sees (modes and rules) and the
// middleware functions that stay here.
function normalizeServices(services) {
  const native = {};
  const handlers = {};
  if (!services) return { native: null, handlers };
  const modeOf = (key, v) => {
    if (typeof v === 'function') {
      handlers[key] = v;
      return 'js';
    }
    if (v === false || v === null) return 'none';
    return MODES[v] || 'inherit';
  };
  for (const key of ['audio', 'video']) if (key in services) native[key] = modeOf(key, services[key]);
  const load = services.load;
  if (load && typeof load === 'object') {
    native.load = typeof load.handle === 'function' ? modeOf('load', load.handle) : 'inherit';
    if (Array.isArray(load.allow)) native.allow = load.allow.map(String);
    if (Array.isArray(load.deny)) native.deny = load.deny.map(String);
    const rw = load.rewrite;
    if (rw) native.rewrite = (Array.isArray(rw) ? rw : Object.entries(rw)).map(([a, b]) => [String(a), String(b)]);
  } else if (load !== undefined) {
    native.load = modeOf('load', load);
  }
  if ('fonts' in services) native.fonts = modeOf('fonts', services.fonts) !== 'none';
  const tex = services.textures;
  if (tex && typeof tex === 'object') {
    native.textures = tex.inherit !== false;
    if (tex.alias) native.textureAliases = { ...tex.alias };
  } else if (tex !== undefined) {
    native.textures = modeOf('textures', tex) !== 'none';
  }
  return { native, handlers };
}

// Runs one middleware: exactly one of pass(changed) / reply(answer) / block().
function runMiddleware(fn, input, pass, reply, block, onError) {
  let settled = false;
  const once = (f) => (v) => {
    if (settled) return;
    settled = true;
    f(v);
  };
  const next = once((changed) => pass(changed === undefined ? input : changed));
  const finish = (out) => {
    if (settled) return;
    if (out === undefined) next();
    else if (out === null || out === false) once(block)();
    else once(reply)(out);
  };
  const fail = (err) => {
    if (onError) onError(String((err && err.message) || err));
    once(block)();
  };
  let out;
  try {
    out = fn(input, next);
  } catch (err) {
    fail(err);
    return;
  }
  if (out && typeof out.then === 'function') out.then(finish, fail);
  else finish(out);
}

export function JSUIContainer(props) {
  const hostRef = useRef(null);
  const latest = useRef(props);
  const lastServices = useRef({ key: '', value: null });
  latest.current = props;
  // Calls the parent made into the child (ref.call): seq -> {resolve, reject}.
  const calls = useRef(null);
  if (!calls.current) calls.current = { seq: 0, pending: new Map() };

  const send = (cmd) => {
    const inst = hostRef.current;
    return !!(inst && hasNative && native.containerCommand(inst.id, cmd));
  };

  useImperativeHandle(props.ref, () => {
    const inst = hostRef.current;
    if (!inst) return null;
    // The ref is the host instance (measure, focus, ...) with the embedder's
    // side of the child's Host API on it.
    inst.postMessage = (name, payload) => send({ op: 'post', name: String(name), payload: json(payload) });
    inst.call = (name, args) =>
      new Promise((resolve, reject) => {
        const c = calls.current;
        const seq = ++c.seq;
        c.pending.set(seq, { resolve, reject });
        if (!send({ op: 'call', seq, name: String(name), args: json(args) })) {
          c.pending.delete(seq);
          reject(new Error('JSUIContainer: not mounted'));
        }
      });
    inst.setState = (key, value) =>
      send(value === undefined ? { op: 'state', key: String(key), remove: true } : { op: 'state', key: String(key), value });
    inst.patchState = (key, patch) => send({ op: 'state', key: String(key), value: json(patch), patch: true });
    inst.reload = () => send({ op: 'reload' });
    return inst;
  });

  // Inline source goes to native once per change, not as a prop the bridge
  // would copy on every update.
  const { bundleData } = props;
  useLayoutEffect(() => {
    if (typeof bundleData === 'string' && bundleData) send({ op: 'load', data: bundleData });
  }, [bundleData]);

  // Calls still waiting when the container goes away fail.
  useLayoutEffect(
    () => () => {
      const c = calls.current;
      for (const p of c.pending.values()) p.reject(new Error('JSUIContainer: unmounted'));
      c.pending.clear();
    },
    [],
  );

  const out = {};
  for (const k in props) if (OMIT[k] !== 1) out[k] = props[k];
  out.ref = hostRef;
  if (!hasNative) {
    warnOnce('container-unavailable', 'JSUIContainer: this host cannot run nested JSUI bundles; showing an empty view');
    return jsx('View', out);
  }
  if (props.initialProps != null) out.initialProps = props.initialProps;
  // The same rules keep the same object (no prop update for the bridge).
  const services = normalizeServices(props.services).native;
  const servicesKey = services ? JSON.stringify(services) : '';
  if (lastServices.current.key !== servicesKey) lastServices.current = { key: servicesKey, value: services };
  if (lastServices.current.value) out.services = lastServices.current.value;
  if (props.hostState != null) out.hostState = props.hostState;
  out.onContainerEvent = (e) => {
    const ev = e.nativeEvent;
    const p = latest.current;
    switch (ev.kind) {
      case 'message':
        if (p.onMessage) p.onMessage(ev.name, ev.payload);
        break;
      case 'state':
        if (p.onStateChange) p.onStateChange(ev.key, ev.removed ? undefined : ev.value);
        break;
      case 'call': {
        const { callId, name, args } = ev;
        const handler = p.onCall;
        if (!handler) {
          send({ op: 'reject', callId, message: 'no onCall handler for ' + name });
          break;
        }
        new Promise((resolve) => resolve(handler(name, args, { callId }))).then(
          (value) => send({ op: 'resolve', callId, value: json(value) }),
          (err) => send({ op: 'reject', callId, message: String((err && err.message) || err) }),
        );
        break;
      }
      case 'callCancel':
        if (p.onCallCancel) p.onCallCancel(ev.callId);
        break;
      case 'callResult': {
        const c = calls.current.pending.get(ev.seq);
        if (!c) break;
        calls.current.pending.delete(ev.seq);
        if (ev.ok) c.resolve(ev.result);
        else c.reject(new Error(ev.error));
        break;
      }
      case 'error':
        if (p.onError) p.onError(ev.message);
        break;
      case 'load': {
        const { gen, id } = ev;
        const fn = normalizeServices(p.services).handlers.load;
        if (!fn) {
          send({ op: 'load-next', gen, id, request: {} });
          break;
        }
        runMiddleware(
          fn,
          ev.request,
          (request) => send({ op: 'load-next', gen, id, request }),
          (r) =>
            send(
              typeof r === 'string'
                ? { op: 'load-reply', gen, id, status: 200, body: r }
                : {
                    op: 'load-reply', gen, id,
                    status: r.status == null ? 200 : r.status,
                    statusText: r.statusText || '',
                    headers: r.headers || {},
                    body: r.body == null ? '' : typeof r.body === 'string' ? r.body : JSON.stringify(r.body),
                  },
            ),
          () => send({ op: 'load-block', gen, id }),
          p.onError,
        );
        break;
      }
      case 'audio':
      case 'video': {
        const { gen, cmd } = ev;
        const fn = normalizeServices(p.services).handlers[ev.kind];
        const pass = (changed) => send({ op: ev.kind + '-next', gen, orig: cmd, cmd: changed });
        if (!fn) {
          pass(cmd);
          break;
        }
        // An object returned is the command to pass on.
        runMiddleware(fn, cmd, pass, pass, () => send({ op: ev.kind + '-block', gen, orig: cmd, cmd: {} }), p.onError);
        break;
      }
    }
  };
  return jsx('JSUIContainer', out);
}
JSUIContainer.displayName = 'JSUIContainer';

export default JSUIContainer;
