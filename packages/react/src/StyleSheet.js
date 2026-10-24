// StyleSheet: RN-compatible create/flatten/compose plus the style helpers the
// host config uses to keep bridge traffic low (flatten + deep compare).

const absoluteFillObject = Object.freeze({ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 });

/**
 * Flattens a style (object, falsy, or nested arrays) into one object.
 * A single object is returned as is (no copy), so stable StyleSheet entries
 * stay referentially equal across renders and diffing is O(1) for them.
 * @returns {object|undefined}
 */
export function flatten(style) {
  if (style == null || style === false || style === true || style === '') return undefined;
  if (!Array.isArray(style)) return typeof style === 'object' ? style : undefined;
  // Module state instead of a closure per call (saved, in case a style
  // getter flattens too).
  const c = flatCount, s1 = flatSingle, o = flatOut;
  flatCount = 0;
  flatSingle = flatOut = undefined;
  flattenArray(style);
  const result = flatCount === 0 ? undefined : flatCount === 1 ? flatSingle : flatOut;
  flatCount = c;
  flatSingle = s1;
  flatOut = o;
  return result;
}

let flatCount = 0;
let flatSingle;  // avoid copying when only one entry is set
let flatOut;
function flattenArray(arr) {
  for (let i = 0; i < arr.length; i++) {
    const s = arr[i];
    if (s == null || s === false || s === true || s === '') continue;
    if (Array.isArray(s)) {
      flattenArray(s);
      continue;
    }
    if (typeof s !== 'object') continue;
    flatCount++;
    if (flatCount === 1) {
      flatSingle = s;
      continue;
    }
    if (flatCount === 2) flatOut = Object.assign({}, flatSingle);
    const out = flatOut;
    for (const k in s) {
      const v = s[k];
      if (v !== undefined) out[k] = v;
    }
  }
}

/** Structural equality for JSON-like values (styles, sources, ...). */
export function deepEqual(a, b, depth = 0) {
  if (a === b) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) {
    return typeof a === 'number' && typeof b === 'number' && a !== a && b !== b;  // NaN
  }
  if (depth > 16) return false;
  const aa = Array.isArray(a);
  if (aa !== Array.isArray(b)) return false;
  if (aa) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i], depth + 1)) return false;
    return true;
  }
  let n = 0;
  for (const k in a) {
    if (a[k] === undefined) continue;
    n++;
    if (!deepEqual(a[k], b[k], depth + 1)) return false;
  }
  for (const k in b) if (b[k] !== undefined) n--;
  return n === 0;
}

function create(styles) {
  // RN returns the same objects (since 0.70 no numeric ids). Freeze in dev so
  // accidental mutation (which would defeat reference-equality diffing) throws.
  if (process.env.NODE_ENV !== 'production') {
    for (const k in styles) if (styles[k] && typeof styles[k] === 'object') Object.freeze(styles[k]);
  }
  return styles;
}

function compose(a, b) {
  if (a != null && b != null) return [a, b];
  return a != null ? a : b;
}

export const StyleSheet = {
  create,
  flatten,
  compose,
  hairlineWidth: 1,
  absoluteFill: absoluteFillObject,
  absoluteFillObject,
  setStyleAttributePreprocessor() {},
};

export default StyleSheet;
