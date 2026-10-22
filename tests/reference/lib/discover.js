// Finds example cases and reads their `meta` export without a React runtime.
import fs from 'node:fs';
import path from 'node:path';
import { EXAMPLES_DIR } from './paths.js';

// Example groups, in report order. Each *.jsx directly inside is one case.
const GROUPS = ['features', 'screens'];

export const DEFAULT_META = { width: 800, height: 600, maxMismatch: 1.5, waitMs: 300 };

// `meta` must be a plain object literal (`export const meta = { ... };`). It is
// read textually so no app code (or React) has to run in Node.
function readMeta(file) {
  const src = fs.readFileSync(file, 'utf8');
  const m = /export\s+const\s+meta\s*=\s*\{/.exec(src);
  if (!m) return { ...DEFAULT_META };
  const begin = m.index + m[0].length - 1;
  let depth = 0;
  let i = begin;
  for (; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}' && --depth === 0) break;
  }
  try {
    return { ...DEFAULT_META, ...new Function(`return (${src.slice(begin, i + 1)});`)() };
  } catch (e) {
    throw new Error(`${file}: meta must be a plain object literal (${e.message})`);
  }
}

/** @returns {Promise<Array<{name:string, group:string, file:string, meta:object}>>} */
export async function discoverCases(filters = []) {
  const cases = [];
  for (const group of GROUPS) {
    const dir = path.join(EXAMPLES_DIR, group);
    if (!fs.existsSync(dir)) continue;
    for (const f of fs.readdirSync(dir).sort()) {
      if (!/^[^.].*\.(jsx|js)$/.test(f)) continue;
      const name = `${group}/${f.replace(/\.(jsx|js)$/, '')}`;
      if (filters.length && !filters.some((p) => name.includes(p))) continue;
      const file = path.join(dir, f);
      cases.push({ name, group, file, meta: readMeta(file) });
    }
  }
  return cases;
}
