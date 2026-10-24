// Source map v3 helpers for the HMR bundle (hmr.js): VLQ decoding/encoding
// and a builder that concatenates per-part maps into one flattened map.
//
// Sources are absolute file:// URLs (see fileSource): the maps are read by
// DevTools and VS Code from http:// (/bundle.js.map) and from data: URLs
// (hot updates), against which a relative path would not resolve to the
// file on disk.

import path from 'node:path';
import { pathToFileURL } from 'node:url';

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64_VALUE = new Int8Array(128).fill(-1);
for (let i = 0; i < B64.length; i++) B64_VALUE[B64.charCodeAt(i)] = i;

/**
 * Decodes `mappings` into lines of segments with absolute values:
 * [genColumn] or [genColumn, source, origLine, origColumn(, name)].
 */
export function decodeMappings(mappings) {
  const lines = [];
  let line = [];
  let src = 0;
  let oLine = 0;
  let oCol = 0;
  let name = 0;
  let gCol = 0;
  const seg = [];
  let value = 0;
  let shift = 0;
  const flush = () => {
    if (!seg.length) return;
    gCol += seg[0];
    if (seg.length >= 4) {
      src += seg[1];
      oLine += seg[2];
      oCol += seg[3];
      if (seg.length >= 5) {
        name += seg[4];
        line.push([gCol, src, oLine, oCol, name]);
      } else line.push([gCol, src, oLine, oCol]);
    } else line.push([gCol]);
    seg.length = 0;
  };
  for (let i = 0; i < mappings.length; i++) {
    const c = mappings.charCodeAt(i);
    if (c === 59 /* ; */) {
      flush();
      lines.push(line);
      line = [];
      gCol = 0;
    } else if (c === 44 /* , */) {
      flush();
    } else {
      const d = c < 128 ? B64_VALUE[c] : -1;
      if (d < 0) throw new Error(`invalid source map mappings character ${JSON.stringify(mappings[i])}`);
      value += (d & 31) * 2 ** shift;
      if (d & 32) {
        shift += 5;
      } else {
        seg.push(value % 2 === 1 ? -(value - 1) / 2 : value / 2);
        value = 0;
        shift = 0;
      }
    }
  }
  flush();
  lines.push(line);
  return lines;
}

function vlq(n) {
  let v = n < 0 ? (-n << 1) | 1 : n << 1;
  let out = '';
  do {
    let d = v & 31;
    v >>>= 5;
    if (v) d |= 32;
    out += B64[d];
  } while (v);
  return out;
}

/** Encodes lines of absolute segments (decodeMappings' format). */
export function encodeMappings(lines) {
  let out = '';
  let src = 0;
  let oLine = 0;
  let oCol = 0;
  let name = 0;
  for (let l = 0; l < lines.length; l++) {
    if (l) out += ';';
    let gCol = 0;
    const segs = lines[l];
    for (let i = 0; i < segs.length; i++) {
      const s = segs[i];
      if (i) out += ',';
      out += vlq(s[0] - gCol);
      gCol = s[0];
      if (s.length >= 4) {
        out += vlq(s[1] - src) + vlq(s[2] - oLine) + vlq(s[3] - oCol);
        src = s[1];
        oLine = s[2];
        oCol = s[3];
        if (s.length >= 5) {
          out += vlq(s[4] - name);
          name = s[4];
        }
      }
    }
  }
  return out;
}

/** A source of a map, as an absolute file:// URL (relative ones resolve against `base`). */
export function fileSource(source, base) {
  if (/^[a-z][\w+.-]*:/i.test(source) && !/^[a-z]:[\\/]/i.test(source)) return source;  // already a URL
  return pathToFileURL(path.resolve(base, source)).href;
}

/**
 * A parsed map with its mappings decoded and its sources made absolute
 * file:// URLs: { sources, sourcesContent, names, lines }. `base` is the
 * directory relative sources are resolved against (for esbuild's output, the
 * outfile's directory; sourceRoot is honored).
 */
export function prepareMap(map, base) {
  const m = typeof map === 'string' ? JSON.parse(map) : map;
  const root = m.sourceRoot ? path.resolve(base, m.sourceRoot) : base;
  return {
    sources: (m.sources || []).map((s) => fileSource(s || '', root)),
    sourcesContent: (m.sources || []).map((_, i) => (m.sourcesContent && m.sourcesContent[i] != null ? m.sourcesContent[i] : null)),
    names: m.names || [],
    lines: decodeMappings(m.mappings || ''),
  };
}

/** Number of lines of `text` (newlines + 1). */
export function lineCount(text) {
  let n = 1;
  for (let i = text.indexOf('\n'); i >= 0; i = text.indexOf('\n', i + 1)) n++;
  return n;
}

/**
 * Flattens parts into one map. `parts` is [{ line, map }] where `line` is
 * the 0-based line of the generated file the part starts at and `map` a
 * prepareMap() result (or null for a part without mappings). Parts must be
 * in increasing line order and must not overlap.
 */
export function concatMaps(parts, file) {
  const sources = [];
  const sourcesContent = [];
  const sourceIndex = new Map();
  const names = [];
  const nameIndex = new Map();
  const lines = [];
  for (const { line, map } of parts) {
    if (!map) continue;
    const srcMap = map.sources.map((s, i) => {
      let k = sourceIndex.get(s);
      if (k === undefined) {
        k = sources.length;
        sourceIndex.set(s, k);
        sources.push(s);
        sourcesContent.push(map.sourcesContent[i]);
      } else if (sourcesContent[k] == null && map.sourcesContent[i] != null) sourcesContent[k] = map.sourcesContent[i];
      return k;
    });
    const nameMap = map.names.map((n) => {
      let k = nameIndex.get(n);
      if (k === undefined) {
        k = names.length;
        nameIndex.set(n, k);
        names.push(n);
      }
      return k;
    });
    while (lines.length < line) lines.push([]);
    for (let l = 0; l < map.lines.length; l++) {
      const segs = map.lines[l];
      const out = segs.map((s) => (s.length === 1 ? s : s.length === 4 ? [s[0], srcMap[s[1]], s[2], s[3]] : [s[0], srcMap[s[1]], s[2], s[3], nameMap[s[4]]]));
      const at = line + l;
      while (lines.length <= at) lines.push([]);
      lines[at] = lines[at].length ? lines[at].concat(out) : out;
    }
  }
  while (lines.length && !lines[lines.length - 1].length) lines.pop();
  return { version: 3, file, sources, sourcesContent, names, mappings: encodeMappings(lines) };
}

/** `//# sourceMappingURL=data:...` comment line for an inline map. */
export function inlineMapComment(map) {
  return `//# sourceMappingURL=data:application/json;charset=utf-8;base64,${Buffer.from(JSON.stringify(map)).toString('base64')}`;
}
