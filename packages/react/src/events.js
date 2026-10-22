// JS side of the event system (docs/ARCHITECTURE.md "Event objects").
//
// Native calls globalThis.__jsui_dispatchEvent(ev) with a plain object whose
// `target` is a node id. We build the propagation path from our instance map
// (parent links mirror the host tree), run onXxxCapture handlers root ->
// target, then onXxx target -> root, with RN-shaped synthetic events.
// The RN responder system (PanResponder) is layered on pointer events.

import { instances } from './instances.js';
import { now } from './native.js';
import { reconciler, setUpdatePriority, DiscreteEventPriority, ContinuousEventPriority } from './renderer.js';

const g = globalThis;

// native type -> [React prop, bubbles, discrete]
const TYPES = {
  pointerdown: ['onPointerDown', true, true],
  pointermove: ['onPointerMove', true, false],
  pointerup: ['onPointerUp', true, true],
  pointercancel: ['onPointerCancel', true, true],
  pointerenter: ['onPointerEnter', false, false],
  pointerleave: ['onPointerLeave', false, false],
  click: ['onClick', true, true],
  wheel: ['onWheel', true, false],
  keydown: ['onKeyDown', true, true],
  keyup: ['onKeyUp', true, true],
  textinput: ['onTextInput', true, true],
  change: ['onChange', false, true],
  selectionchange: ['onSelectionChange', false, true],
  focus: ['onFocus', false, true],
  blur: ['onBlur', false, true],
  scroll: ['onScroll', false, false],
  load: ['onLoad', false, true],
  error: ['onError', false, true],
  gamepadbutton: ['onGamepadButton', true, true],
  gamepadaxis: ['onGamepadAxis', true, false],
};

// Global listeners for events that are not tied to one node (gamepad, keys
// for modals / BackHandler, resize).
const globalListeners = new Map();
/** Subscribes to every dispatched event of `type`; returns unsubscribe. */
export function addGlobalListener(type, fn) {
  let set = globalListeners.get(type);
  if (!set) globalListeners.set(type, (set = new Set()));
  set.add(fn);
  return () => set.delete(fn);
}

function emitGlobal(type, syn) {
  const set = globalListeners.get(type);
  if (!set || set.size === 0) return false;
  let ran = false;
  for (const fn of Array.from(set)) {
    fn(syn);
    ran = true;
  }
  return ran;
}

class SyntheticEvent {
  constructor(type, ev, target) {
    this.type = type;
    this.target = target;
    this.currentTarget = target;
    this.timeStamp = ev.timestamp != null ? ev.timestamp : now();
    this.defaultPrevented = false;
    this._stopped = false;
    this.bubbles = true;
    this.isTrusted = true;
    const ne = ev;
    if (ev.x !== undefined) {
      ne.pageX = ev.x;
      ne.pageY = ev.y;
      ne.locationX = ev.localX !== undefined ? ev.localX : ev.x;
      ne.locationY = ev.localY !== undefined ? ev.localY : ev.y;
      ne.clientX = ev.x;
      ne.clientY = ev.y;
      ne.identifier = ev.pointerId || 0;
      ne.target = target;
      // RN touch-style fields so responder code reading touches works.
      const touch = { identifier: ne.identifier, locationX: ne.locationX, locationY: ne.locationY, pageX: ne.pageX, pageY: ne.pageY, target, timestamp: this.timeStamp };
      ne.changedTouches = [touch];
      ne.touches = type === 'pointerup' || type === 'pointercancel' ? [] : [touch];
    }
    if (ne.timestamp === undefined) ne.timestamp = this.timeStamp;
    this.nativeEvent = ne;
    // RN-web exposes DOM-ish fields directly on the event.
    if (ev.key !== undefined) {
      this.key = ev.key;
      this.code = ev.code;
      this.repeat = !!ev.repeat;
      this.shiftKey = !!ev.shiftKey;
      this.ctrlKey = !!ev.ctrlKey;
      this.altKey = !!ev.altKey;
      this.metaKey = !!ev.metaKey;
    }
    if (ev.x !== undefined) {
      this.pageX = ev.x;
      this.pageY = ev.y;
      this.clientX = ev.x;
      this.clientY = ev.y;
      this.locationX = ne.locationX;
      this.locationY = ne.locationY;
      this.pointerId = ev.pointerId;
      this.pointerType = ev.pointerType;
      this.button = ev.button;
      this.buttons = ev.buttons;
    }
    if (ev.deltaX !== undefined || ev.deltaY !== undefined) {
      this.deltaX = ev.deltaX || 0;
      this.deltaY = ev.deltaY || 0;
    }
    if (ev.buttonIndex !== undefined || ev.axis !== undefined) {
      this.gamepad = ev.gamepad;
      this.buttonIndex = ev.buttonIndex;
      this.axis = ev.axis;
      this.value = ev.value;
      this.pressed = ev.pressed !== undefined ? !!ev.pressed : ev.down !== undefined ? !!ev.down : ev.value > 0.5;
      this.repeat = !!ev.repeat;
    }
  }
  stopPropagation() {
    this._stopped = true;
  }
  isPropagationStopped() {
    return this._stopped;
  }
  preventDefault() {
    this.defaultPrevented = true;
  }
  isDefaultPrevented() {
    return this.defaultPrevented;
  }
  persist() {}
  isPersistent() {
    return true;
  }
}

export function createSyntheticEvent(type, ev, target) {
  return new SyntheticEvent(type, ev, target);
}

let pendingError = null;
function call(fn, inst, syn) {
  syn.currentTarget = inst.id;
  try {
    fn(syn);
  } catch (e) {
    if (!pendingError) pendingError = e;
  }
}

// --- responder system (subset of RN's ResponderEventPlugin) ---------------
let responder = null;  // HostInstance holding the responder

function pathOf(inst) {
  const path = [];
  for (let i = inst; i; i = i.parent) path.push(i);
  return path;
}

function negotiate(path, startProp, syn) {
  // Capture phase from the root, then bubble from the target.
  for (let i = path.length - 1; i >= 0; i--) {
    const f = path[i].props && path[i].props[startProp + 'Capture'];
    if (f && call2(f, path[i], syn)) return path[i];
  }
  for (let i = 0; i < path.length; i++) {
    const f = path[i].props && path[i].props[startProp];
    if (f && call2(f, path[i], syn)) return path[i];
  }
  return null;
}

function call2(fn, inst, syn) {
  syn.currentTarget = inst.id;
  try {
    return !!fn(syn);
  } catch (e) {
    if (!pendingError) pendingError = e;
    return false;
  }
}

function grant(inst, syn) {
  if (responder && responder !== inst) {
    const req = responder.props.onResponderTerminationRequest;
    if (req && !call2(req, responder, syn)) {
      const rej = inst.props.onResponderReject;
      if (rej) call(rej, inst, syn);
      return false;
    }
    const term = responder.props.onResponderTerminate;
    if (term) call(term, responder, syn);
  }
  responder = inst;
  const gr = inst.props.onResponderGrant;
  if (gr) call(gr, inst, syn);
  const st = inst.props.onResponderStart;
  if (st) call(st, inst, syn);
  return true;
}

function handleResponder(type, path, syn) {
  let ran = false;
  if (responder && !instances.has(responder.id)) responder = null;
  if (type === 'pointerdown') {
    const inst = negotiate(path, 'onStartShouldSetResponder', syn);
    if (inst && inst !== responder) ran = grant(inst, syn) || ran;
    else if (responder) {
      const st = responder.props.onResponderStart;
      if (st) call(st, responder, syn), (ran = true);
    }
  } else if (type === 'pointermove') {
    const ne = syn.nativeEvent;
    if (!responder && (ne.buttons || ne.pointerType === 'touch')) {
      const inst = negotiate(path, 'onMoveShouldSetResponder', syn);
      if (inst) ran = grant(inst, syn) || ran;
    }
    if (responder) {
      const mv = responder.props.onResponderMove;
      if (mv) call(mv, responder, syn), (ran = true);
    }
  } else if (type === 'pointerup' || type === 'pointercancel') {
    if (responder) {
      const r = responder;
      responder = null;
      const end = r.props.onResponderEnd;
      if (end) call(end, r, syn);
      const f = type === 'pointerup' ? r.props.onResponderRelease : r.props.onResponderTerminate;
      if (f) call(f, r, syn);
      ran = true;
    }
  }
  return ran;
}

// --- dispatch --------------------------------------------------------------

/**
 * Entry point installed as globalThis.__jsui_dispatchEvent. Returns a
 * bitmask: 1 = at least one handler ran, 2 = preventDefault() was called
 * (native then skips its default action: click synthesis, focus moves,
 * scrolling, ...). 0 = nobody cared.
 */
export function dispatchEvent(ev) {
  if (!ev || typeof ev !== 'object') return 0;
  const type = ev.type;
  if (type === 'resize') {
    emitGlobal('resize', ev);
    return 1;
  }
  const info = TYPES[type];
  const target = instances.get(ev.target);
  const syn = new SyntheticEvent(type, ev, target ? target.id : ev.target);
  if (info) syn.bubbles = info[1];
  let ran = false;
  const prev = setUpdatePriority(info && !info[2] ? ContinuousEventPriority : DiscreteEventPriority);
  try {
    reconciler.batchedUpdates(() => {
      if (target && info) {
        const prop = info[0];
        if (!info[1]) {
          const f = target.props && target.props[prop];
          if (f) call(f, target, syn), (ran = true);
        } else {
          const path = pathOf(target);
          const cap = prop + 'Capture';
          for (let i = path.length - 1; i >= 0 && !syn._stopped; i--) {
            const f = path[i].props && path[i].props[cap];
            if (f) call(f, path[i], syn), (ran = true);
          }
          for (let i = 0; i < path.length && !syn._stopped; i++) {
            const f = path[i].props && path[i].props[prop];
            if (f) call(f, path[i], syn), (ran = true);
          }
          if (type.charCodeAt(0) === 112 /* p */ && type !== 'pointerenter' && type !== 'pointerleave') {
            ran = handleResponder(type, path, syn) || ran;
          }
        }
      } else if (responder && (type === 'pointermove' || type === 'pointerup' || type === 'pointercancel')) {
        // Pointer left every known node (or was released off-window).
        ran = handleResponder(type, [], syn) || ran;
      }
      // Global listeners see the event unless a node handler stopped it.
      if (!syn._stopped) ran = emitGlobal(type, syn) || ran;
    }, null);
  } finally {
    setUpdatePriority(prev);
  }
  if (pendingError) {
    const e = pendingError;
    pendingError = null;
    throw e;
  }
  return (ran ? 1 : 0) | (syn.defaultPrevented ? 2 : 0);
}

// --- layout ----------------------------------------------------------------

export function dispatchLayout(id, layout) {
  const inst = instances.get(id);
  const f = inst && inst.props && inst.props.onLayout;
  if (!f) return;
  const l = layout || {};
  const e = new SyntheticEvent('layout', { layout: { x: +l.x || 0, y: +l.y || 0, width: +l.width || 0, height: +l.height || 0 } }, id);
  const prev = setUpdatePriority(DiscreteEventPriority);
  try {
    reconciler.batchedUpdates(() => f(e), null);
  } finally {
    setUpdatePriority(prev);
  }
}

// --- host messages -----------------------------------------------------------

const hostListeners = new Map();
export function onHostMessage(name, cb) {
  let set = hostListeners.get(name);
  if (!set) hostListeners.set(name, (set = new Set()));
  set.add(cb);
  return () => set.delete(cb);
}

function deliverHostMessage(name, json) {
  const set = hostListeners.get(name);
  const any = hostListeners.get('*');
  if ((!set || !set.size) && (!any || !any.size)) return;
  let data = json;
  if (typeof json === 'string') {
    try {
      data = json.length ? JSON.parse(json) : undefined;
    } catch (e) {
      data = json;
    }
  }
  // Sync priority: state pushed by the game renders in the same tick
  // (flushed in the microtask checkpoint after this call), not a frame later.
  const prev = setUpdatePriority(DiscreteEventPriority);
  try {
    reconciler.batchedUpdates(() => {
      if (set) for (const cb of Array.from(set)) cb(data, name);
      if (any) for (const cb of Array.from(any)) cb(data, name);
    }, null);
  } finally {
    setUpdatePriority(prev);
  }
}

export function installGlobals() {
  g.__jsui_dispatchEvent = dispatchEvent;
  g.__jsui_onLayout = dispatchLayout;
  g.__jsui_onHostMessage = deliverHostMessage;
}
