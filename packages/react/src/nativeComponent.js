// Native components: view types the host registers by name and draws with
// its own code (include/jsui/jsui.h "Native components"), used the React
// Native way:
//
//   import { requireNativeComponent, codegenNativeCommands } from 'react-native';
//   const Radar = requireNativeComponent('Radar');
//   const RadarCommands = codegenNativeCommands({ supportedCommands: ['ping'] });
//   <Radar ref={ref} range={120} sweep={anim} onContactPress={(e) => pick(e.nativeEvent.id)}
//          style={{ width: 200, height: 200 }} />
//   RadarCommands.ping(ref.current, 40, 60);
//
// The element is the host node "NativeComponentView" with the class name in
// `component`. Plain props cross the bridge as they are (the class reads
// them by name); Animated ones become the paint-only style keys
// `prop:<name>`, so they animate (also with useNativeDriver) with no React
// render. Event props the runtime does not route itself (on<Name>) are
// listed in `__componentEvents`; native emits them as one `componentevent`
// ({name, data}) and this calls on<Name> with nativeEvent = data.
//
// A host that has no class of that name paints the View part only (its
// background, border, children); hasNativeComponent(name) tells.

import { jsx } from 'react/jsx-runtime';
import { native } from './native.js';
import { isRuntimeEvent } from './instances.js';
import { AnimatedNode } from './animated/nodes.js';
import { createAnimatedComponent } from './animated/createAnimatedComponent.js';

const AnimatedHost = createAnimatedComponent('NativeComponentView');

/** Whether the host registered a native component class `name`. */
export function hasNativeComponent(name) {
  return typeof native.hasComponent === 'function' && !!native.hasComponent(String(name));
}

const components = new Map();

/** The React component for native component class `name` (see the top of this file). */
export function requireNativeComponent(name) {
  name = String(name);
  let C = components.get(name);
  if (C) return C;
  C = function NativeComponent(props) {
    const out = { component: name };
    let animated = null;
    let events = null;
    for (const k in props) {
      const v = props[k];
      if (k === 'style' || k === 'children' || k === 'ref') {
        out[k] = v;
        continue;
      }
      if (v instanceof AnimatedNode) {
        (animated || (animated = {}))['prop:' + k] = v;
        continue;
      }
      if (typeof v === 'function' && k.length > 2 && k[0] === 'o' && k[1] === 'n' && k[2] >= 'A' && k[2] <= 'Z' &&
          !isRuntimeEvent(k)) {
        (events || (events = [])).push(k[2].toLowerCase() + k.slice(3));
        continue;
      }
      out[k] = v;
    }
    if (animated) out.style = [props.style, animated];
    if (events) {
      out.__componentEvents = events;
      out.onComponentEvent = (e) => {
        const ne = e.nativeEvent || {};
        const n = ne.name;
        const fn = typeof n === 'string' && n ? props['on' + n[0].toUpperCase() + n.slice(1)] : null;
        if (typeof fn !== 'function') return;
        e.nativeEvent = ne.data != null && typeof ne.data === 'object' ? ne.data : {};
        fn(e);
      };
    }
    // Always through the animated host, so a prop can turn into an Animated
    // value (and back) without remounting the native view.
    return jsx(AnimatedHost, out);
  };
  C.displayName = name;
  components.set(name, C);
  return C;
}

/** codegenNativeComponent (the new architecture's spelling): the same component. */
export function codegenNativeComponent(name) {
  return requireNativeComponent(name);
}

function viewId(ref) {
  if (ref == null) return 0;
  if (typeof ref === 'number') return ref;
  if (typeof ref.id === 'number') return ref.id;
  if (ref.current) return viewId(ref.current);
  return 0;
}

/**
 * Sends command `name` with `args` (an array) to a native component view
 * (its ref, host instance or id). Returns whether its class took it.
 */
export function dispatchCommand(ref, name, args) {
  const id = viewId(ref);
  if (!id || typeof native.componentCommand !== 'function') return false;
  return !!native.componentCommand(id, String(name), Array.isArray(args) ? args : []);
}

/**
 * codegenNativeCommands({ supportedCommands: ['zoomTo', ...] }): an object
 * with one function per command, (ref, ...args) => dispatchCommand(ref, name, args).
 * Without the list, any name works.
 */
export function codegenNativeCommands(options) {
  const list = options && Array.isArray(options.supportedCommands) ? options.supportedCommands : null;
  const make = (name) => (ref, ...args) => dispatchCommand(ref, name, args);
  if (list) {
    const out = {};
    for (const n of list) out[n] = make(n);
    return out;
  }
  return new Proxy({}, { get: (_, n) => (typeof n === 'string' ? make(n) : undefined) });
}
