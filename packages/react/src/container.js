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
//     style={{ flex: 1 }}
//   />
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
  bundleData: 1, hostState: 1, initialProps: 1, ref: 1,
  onMessage: 1, onCall: 1, onCallCancel: 1, onStateChange: 1, onError: 1,
};

const json = (v) => (v === undefined ? null : v);

export function JSUIContainer(props) {
  const hostRef = useRef(null);
  const latest = useRef(props);
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
    }
  };
  return jsx('JSUIContainer', out);
}
JSUIContainer.displayName = 'JSUIContainer';

export default JSUIContainer;
