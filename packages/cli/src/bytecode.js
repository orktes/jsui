// Precompiles bundles to QuickJS bytecode with the WebAssembly runtime: the
// same patched QuickJS as every native runtime of this JSUI version, so the
// bytecode matches theirs (src/js/bytecode.h has the container format; a
// runtime built from other QuickJS sources runs the embedded source instead).

import { loadModule } from './headless.js';

/** True when `bytes` (a Buffer / Uint8Array) is a precompiled bundle. */
export function isPrecompiled(bytes) {
  const magic = [0, 0x4a, 0x53, 0x55, 0x49, 0x42, 0x43, 1]; // "\0JSUIBC\1"
  return bytes.length >= 8 && magic.every((b, i) => bytes[i] === b);
}

/**
 * The parts of a precompiled bundle: {key, bytecode, sourceUrl, source}
 * (source '' when it is not inside), or null for a plain JS bundle.
 */
export function readPrecompiled(bytes) {
  const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (!isPrecompiled(buf)) return null;
  let at = 8;
  const block = () => {
    const n = buf.readUInt32LE(at);
    const b = buf.subarray(at + 4, at + 4 + n);
    at += 4 + n;
    return b;
  };
  const key = block().toString('utf8');
  const bytecode = block();
  const sourceUrl = block().toString('utf8');
  return { key, bytecode, sourceUrl, source: buf.subarray(at).toString('utf8') };
}

/** The embedded source of a precompiled bundle ('' when it has none), or the bundle itself. */
export function bundleSource(bytes) {
  const parts = readPrecompiled(bytes);
  return parts ? parts.source : Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('utf8');
}

/**
 * Compiles a bundle's source to a precompiled bundle (Buffer). `keepSource`
 * puts the source inside as the fallback; `sourceUrl` (relative to the
 * bundle) names a separate file with it instead.
 * Throws with the QuickJS syntax error, or when the WebAssembly runtime is
 * not installed (err.code === 'JSUI_NO_WASM').
 */
export async function compileBundle(source, { name = 'bundle.js', keepSource = true, sourceUrl = null, wasmDir } = {}) {
  let M;
  try {
    M = await loadModule(wasmDir);
  } catch (e) {
    e.code = 'JSUI_NO_WASM';
    throw e;
  }
  if (!M._jsui_compile_bundle) {
    const e = new Error('the installed @orktes/jsui-web is too old to precompile bundles');
    e.code = 'JSUI_NO_WASM';
    throw e;
  }
  const src = Buffer.isBuffer(source) ? source : Buffer.from(source, 'utf8');
  const nameLen = M.lengthBytesUTF8(name) + 1;
  const urlLen = sourceUrl ? M.lengthBytesUTF8(sourceUrl) + 1 : 0;
  const sp = M._malloc(src.length + 1);
  const np = M._malloc(nameLen);
  const up = urlLen ? M._malloc(urlLen) : 0;
  const out = M._malloc(12); // data*, size_t, char* (wasm32)
  try {
    M.HEAPU8.set(src, sp);
    M.HEAPU8[sp + src.length] = 0;
    M.stringToUTF8(name, np, nameLen);
    if (up) M.stringToUTF8(sourceUrl, up, urlLen);
    const r = M._jsui_compile_bundle(sp, src.length, np, keepSource ? 1 : 0, up, out, out + 4, out + 8);
    const data = M.HEAPU32[out >> 2];
    const size = M.HEAPU32[(out + 4) >> 2];
    const err = M.HEAPU32[(out + 8) >> 2];
    if (r !== 0) {
      const msg = err ? M.UTF8ToString(err) : 'compilation failed';
      if (err) M._jsui_free(err);
      throw new Error(msg);
    }
    const bytes = Buffer.from(M.HEAPU8.slice(data, data + size));
    M._jsui_free(data);
    return bytes;
  } finally {
    M._free(sp);
    M._free(np);
    if (up) M._free(up);
    M._free(out);
  }
}
