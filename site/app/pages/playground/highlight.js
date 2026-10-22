// A small JS/JSX tokenizer for the playground editor. Tokenizes the whole
// file (so block comments and template literals span lines) and splits the
// runs per line, so each editor row can be memoized on its own runs.
export const PALETTE = {
  kw: '#c792ea',
  str: '#c3e88d',
  num: '#f78c6c',
  com: '#5c6a8f',
  fn: '#82aaff',
  tag: '#ffcb6b',
  attr: '#89ddff',
  punct: '#8a96b8',
  plain: '#d6def5',
  type: '#ffcb6b',
};

const KW = new Set(
  'import export from default function return const let var if else for while of in new class extends async await try catch finally throw typeof instanceof null undefined true false this switch case break continue'.split(
    ' ',
  ),
);

const TOKEN =
  /(\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:[^"\\\n]|\\.)*"?|'(?:[^'\\\n]|\\.)*'?|`(?:[^`\\]|\\.)*`?)|(\b\d+(?:\.\d+)?\b)|(<\/?[A-Za-z][\w.]*|\/?>)|([A-Za-z_$][\w$]*)(?==)|([A-Za-z_$][\w$]*)(?=\s*\()|([A-Za-z_$][\w$]*)|([{}()[\];,.=+\-*/<>!&|?:%^~@])/g;

function tokenize(code) {
  const out = [];
  let last = 0;
  let m;
  TOKEN.lastIndex = 0;
  while ((m = TOKEN.exec(code))) {
    if (m[0] === '') {
      TOKEN.lastIndex++;
      continue;
    }
    if (m.index > last) out.push(['plain', code.slice(last, m.index)]);
    const [s, com, str, num, tag, attr, fn, word] = m;
    let kind = 'punct';
    if (com) kind = 'com';
    else if (str) kind = 'str';
    else if (num) kind = 'num';
    else if (tag) kind = 'tag';
    else if (attr) kind = 'attr';
    else if (fn) kind = KW.has(fn) ? 'kw' : 'fn';
    else if (word) kind = KW.has(word) ? 'kw' : /^[A-Z]/.test(word) ? 'type' : 'plain';
    out.push([kind, s]);
    last = m.index + s.length;
  }
  if (last < code.length) out.push(['plain', code.slice(last)]);
  return out;
}

/** -> array of lines, each an array of [kind, text] runs. */
export function highlightLines(code) {
  const lines = [[]];
  for (const [kind, text] of tokenize(code)) {
    const parts = text.split('\n');
    parts.forEach((p, i) => {
      if (i > 0) lines.push([]);
      if (p) lines[lines.length - 1].push([kind, p]);
    });
  }
  return lines;
}

/** Stable key for a line's runs (memo comparison). */
export const runsKey = (runs) => runs.map((r) => r[0] + '\u0001' + r[1]).join('\u0002');
