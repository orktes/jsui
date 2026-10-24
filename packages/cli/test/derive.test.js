// jsui build's Animated.derive pass (src/derive.js): which calls it rewrites,
// what it captures, and the build-time warnings.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { maskCode, transformDerive } from '../src/derive.js';

test('rewrites inline derive / expr functions with source and scope', async () => {
  const src = [
    "const w = Animated.derive([v], (x) => clamp(x * K, 0, 100));",
    'const e = Animated.expr((a, b) => a + b);',
    'const n = derive(inputs, function (a) { const k = a * 2; return k + OFF; });',
    'const f = Animated.derive([v], named);',  // not inline: untouched
    "const s = 'Animated.derive([v], (x) => x)';",  // in a string
    '// Animated.derive([v], (x) => x)',
    'const t = `${Animated.derive([v], (x) => x + 1)}`;',
  ].join('\n');
  const { code, warnings } = await transformDerive(src, 'app.jsx');
  assert.deepEqual(warnings, []);
  const lines = code.split('\n');
  assert.match(lines[0], /\(x\) => clamp\(x \* K, 0, 100\), \{ source: "\(x\) => clamp\(x \* K, 0, 100\)", scope: \{ (K|clamp): /);
  assert.match(lines[0], /K: typeof K !== "undefined" \? K : void 0/);
  assert.match(lines[1], /a \+ b, \{ source: "\(a, b\) => a \+ b", scope: \{  \} \}\);$/);
  assert.match(lines[2], /scope: \{ OFF: /);
  assert.doesNotMatch(lines[2], /scope: \{[^}]*\bk:/);
  assert.equal(lines[3], 'const f = Animated.derive([v], named);');
  assert.equal(lines[4], src.split('\n')[4]);
  assert.equal(lines[5], src.split('\n')[5]);
  assert.match(lines[6], /source: "\(x\) => x \+ 1"/);
  // The rewritten code still parses.
  new Function(code.replace(/Animated\.|derive\(|clamp/g, (m) => m));
});

test('functions outside the subset are reported, not rewritten', async () => {
  const src = 'Animated.derive([v], (x) => [x].length);';
  const { code, warnings } = await transformDerive(src, 'app.jsx');
  assert.equal(code, src);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].text, /cannot run natively/);
  assert.equal(warnings[0].location.line, 1);
});

test('maskCode blanks strings, comments and template text only', () => {
  const src = "a('x(') /* y( */ + `t${b('z')}u` // c(";
  const m = maskCode(src);
  assert.equal(m.length, src.length);
  assert.equal(m.replace(/ +/g, ' '), "a(' ') + ` ${b(' ')} ` ");
});
