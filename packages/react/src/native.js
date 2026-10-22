// Access to the __jsui_native bridge installed by the JSUI runtime.
//
// The bridge is read once at module init (it never changes during the
// lifetime of a JS context; a reload creates a new context). Every optional
// function is feature-detected so the package also loads in environments
// that only implement the skeleton bridge (and in Node tests, which install a
// mock before importing).

const g = globalThis;

/** @returns {any} */
function missingBridge() {
  let next = 2;
  const warn = () => {};
  return {
    rootId: 1,
    createNode: () => next++,
    setProps: warn,
    setText: warn,
    appendChild: warn,
    insertBefore: warn,
    removeChild: warn,
    destroyNode: warn,
  };
}

export const native = g.__jsui_native || missingBridge();


const warned = new Set();
/** Logs a warning once per key. */
export function warnOnce(key, message) {
  if (warned.has(key)) return;
  warned.add(key);
  if (g.console) console.warn(message || key);
}

// Minimal polyfills for hosts that only implement the skeleton globals.
export const queueMicrotask =
  typeof g.queueMicrotask === 'function'
    ? g.queueMicrotask.bind(g)
    : (fn) => {
        Promise.resolve().then(fn);
      };

export const now =
  g.performance && typeof g.performance.now === 'function' ? () => g.performance.now() : () => Date.now();

export const requestFrame =
  typeof g.requestAnimationFrame === 'function'
    ? (cb) => g.requestAnimationFrame(cb)
    : (cb) => setTimeout(() => cb(now()), 16);
export const cancelFrame =
  typeof g.cancelAnimationFrame === 'function' ? (id) => g.cancelAnimationFrame(id) : (id) => clearTimeout(id);

/** Viewport {width, height, scale}; reads the live value from the bridge. */
export function readViewport() {
  let v = native.viewport;
  if (typeof v === 'function') v = v();
  if (v && typeof v === 'object' && v.width > 0) {
    return { width: +v.width, height: +v.height, scale: +(v.scale || v.pixelRatio || 1) };
  }
  // Browser-style globals, if the host defines them.
  if (g.innerWidth > 0 && g.innerHeight > 0) {
    return { width: +g.innerWidth, height: +g.innerHeight, scale: +(g.devicePixelRatio || 1) };
  }
  return { width: 1280, height: 720, scale: 1 };
}
