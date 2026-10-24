// Animated.derive at build time.
//
// The runtime compiles a derive function's body into a native expression
// program (@orktes/jsui-react src/animated/expression.js). It needs the
// function's source text, which bytecode bundles do not keep
// (Function#toString gives "[native code]"), and the values of the names it
// reads from the enclosing scope (constants, Animated nodes, helpers). This
// pass rewrites
//
//   Animated.derive([a, b], (x, y) => clamp(x * K, 0, y))
//
// in app source files into
//
//   Animated.derive([a, b], (x, y) => clamp(x * K, 0, y), { source: "(x, y) => ...", scope: { clamp: ..., K: ... } })
//
// and the same for Animated.expr(fn) (an expression defined once and passed
// to derive later),
// using the runtime's own parser, so a function outside the expression
// subset is reported at build time with its file and line. It works on the
// text (no Babel parse) and only touches files that mention `derive(`.

import { pathToFileURL } from 'node:url';
import path from 'node:path';
import { jsuiReactDir } from './config.js';

let parserPromise = null;
function parser() {
  if (!parserPromise) {
    const file = path.join(jsuiReactDir(), 'src', 'animated', 'expression.js');
    parserPromise = import(pathToFileURL(file).href);
  }
  return parserPromise;
}

/**
 * `src` with the contents of comments, strings and template literal text
 * blanked (same length), so a scan for `derive(` and bracket matching only
 * see code. JSX text is not recognised (rarely holds `derive(`).
 */
export function maskCode(src) {
  const out = src.split('');
  const n = src.length;
  const blank = (a, b) => {
    for (let k = a; k < b; k++) if (out[k] !== '\n') out[k] = ' ';
  };
  const stack = [];  // template nesting: brace depth at each `${`
  let depth = 0;
  let i = 0;
  const template = () => {
    // From just after a ` (or a `}` closing ${...}) to the next ` or ${.
    const start = i;
    while (i < n && src[i] !== '`') {
      if (src[i] === '\\') i += 2;
      else if (src[i] === '$' && src[i + 1] === '{') {
        blank(start, i);
        stack.push(depth);
        depth = 0;
        i += 2;
        return;
      } else i++;
    }
    blank(start, i);
    i++;
  };
  while (i < n) {
    const c = src[i];
    if (c === '/' && src[i + 1] === '/') {
      const e = src.indexOf('\n', i);
      const end = e < 0 ? n : e;
      blank(i, end);
      i = end;
    } else if (c === '/' && src[i + 1] === '*') {
      const e = src.indexOf('*/', i + 2);
      const end = e < 0 ? n : e + 2;
      blank(i, end);
      i = end;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c && src[j] !== '\n') j += src[j] === '\\' ? 2 : 1;
      blank(i + 1, Math.min(j, n));
      i = j + 1;
    } else if (c === '`') {
      i++;
      template();
    } else if (c === '{') {
      depth++;
      i++;
    } else if (c === '}') {
      if (depth === 0 && stack.length) {
        depth = stack.pop();
        i++;
        template();
      } else {
        depth--;
        i++;
      }
    } else i++;
  }
  return out.join('');
}

// derive(inputs, fn) (any callee named derive) and X.expr(fn).
const CALL = /(^|[^\w$])derive\s*\(|\.expr\s*\(/g;

function skipSpace(masked, i) {
  while (i < masked.length && /\s/.test(masked[i])) i++;
  return i;
}

/**
 * Rewrites the derive calls of one source file. Returns {code, warnings}
 * (code === src when nothing changed). `file` is for messages only.
 */
export async function transformDerive(src, file) {
  const warnings = [];
  if (!src.includes('derive')) return { code: src, warnings };
  const masked = maskCode(src);
  CALL.lastIndex = 0;
  if (!CALL.test(masked)) return { code: src, warnings };
  const { parseExpressionFunction } = await parser();
  const edits = [];
  CALL.lastIndex = 0;
  for (let m; (m = CALL.exec(masked)); ) {
    const open = m.index + m[0].length - 1;
    let fnStart;
    if (m[0].startsWith('.expr')) {
      fnStart = skipSpace(masked, open + 1);
    } else {
      // Skip the first argument: to the first ',' at depth 0.
      let i = open + 1, d = 0;
      for (; i < masked.length; i++) {
        const c = masked[i];
        if (c === '(' || c === '[' || c === '{') d++;
        else if (c === ')' || c === ']' || c === '}') {
          if (d === 0) break;
          d--;
        } else if (c === ',' && d === 0) break;
      }
      if (masked[i] !== ',') continue;
      fnStart = skipSpace(masked, i + 1);
    }
    if (!/^(\(|function\b|[A-Za-z_$][\w$]*\s*=>)/.test(masked.slice(fnStart, fnStart + 40))) continue;  // not an inline function
    let parsed;
    try {
      parsed = parseExpressionFunction(src, fnStart);
    } catch (e) {
      const line = src.slice(0, fnStart).split('\n').length;
      warnings.push({ text: `Animated.derive function cannot run natively: ${e.message}`, location: { file, line } });
      continue;
    }
    const after = skipSpace(masked, parsed.end);
    if (masked[after] !== ')') continue;  // already has the meta argument
    const source = src.slice(fnStart, parsed.end).trim();
    const scope = parsed.free.map((k) => `${k}: typeof ${k} !== "undefined" ? ${k} : void 0`).join(', ');
    edits.push({ at: parsed.end, text: `, { source: ${JSON.stringify(source)}, scope: { ${scope} } }` });
  }
  if (!edits.length) return { code: src, warnings };
  let code = src;
  for (let k = edits.length - 1; k >= 0; k--) code = code.slice(0, edits[k].at) + edits[k].text + code.slice(edits[k].at);
  return { code, warnings };
}

/**
 * esbuild plugin: the transform for source files no other plugin loads
 * (app files, the CLI's test fixtures, packages; not @orktes/jsui-react's
 * own sources).
 */
export function derivePlugin({ reactDir }) {
  const own = path.join(reactDir, 'src') + path.sep;
  const isApp = (p) => !p.startsWith(own) && !p.includes(`${path.sep}node_modules${path.sep}react`);
  return {
    name: 'jsui-derive',
    setup(build) {
      build.onLoad({ filter: /\.(jsx?|tsx?|mjs|cjs)$/i, namespace: 'file' }, async (args) => {
        if (!isApp(args.path)) return undefined;
        const { readFile } = await import('node:fs/promises');
        const src = await readFile(args.path, 'utf8');
        const r = await transformDerive(src, args.path);
        if (r.code === src && !r.warnings.length) return undefined;
        const ext = path.extname(args.path).slice(1).toLowerCase();
        return { contents: r.code, warnings: r.warnings, loader: ext === 'ts' || ext === 'tsx' ? ext : ext === 'json' ? 'json' : 'jsx', resolveDir: path.dirname(args.path) };
      });
    },
  };
}
