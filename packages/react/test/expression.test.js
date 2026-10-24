// Animated.derive's expression compiler (src/animated/expression.js): the
// accepted subset, its errors, and evaluation against plain JS.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compileExpression, evaluateExpression, parseExpressionFunction, Expr, clamp } from '../src/animated/expression.js';

const run = (fn, inputs, scope) => {
  const p = compileExpression(String(fn), inputs.length, scope);
  return evaluateExpression(p.code, inputs, p.slots);
};

test('expressions evaluate like the JS function', () => {
  const cases = [
    (a, b) => a + b * 2 - a / (b + 1),
    (a, b) => (a % 3) + a ** 2 - -b,
    (a, b) => (a > b ? a : b) + (a <= b && b !== 3 ? 1 : 2) + (!a || b),
    (a, b) => Math.min(a, b, 4) + Math.max(a) + Math.hypot(a, b) + Math.atan2(a, b) + Math.round(-2.5) + Math.sign(a - b),
    (a, b) => {
      const d = a - b;
      let e = d * 2;
      if (d > 1) return e;
      else if (d < -1) {
        const f = d * 3;
        return f;
      }
      return Math.PI * d;
    },
    function (a, b) {
      if (a === b) return 0;
      return a < b ? -1 : 1;
    },
    (a) => 0x10 + 1_000 + 1e2 + .5 + (true ? 1 : 0) + (false ? 1 : 0),
  ];
  for (const fn of cases)
    for (const [a, b] of [[0, 0], [1, 2], [5, -3], [2.5, 2.5], [-7, 0.25]]) {
      const want = +fn(a, b);
      assert.equal(run(fn, [a, b]), want === want ? want : 0, `${fn} (${a}, ${b})`);
    }
});

test('helpers and captured scope', () => {
  assert.equal(run((x) => clamp(x, 0, 1) + Expr.smoothstep(0, 10, x) + Expr.wrap(x, 3) + Expr.fract(x) + Expr.mix(0, 10, 0.5), [5]), 1 + 0.5 + 2 + 0 + 5);
  assert.equal(run((x) => x * K, [2], { K: 21 }), 42);
  assert.throws(() => run((x) => x * K, [2]), /'K' is not defined/);
  assert.throws(() => run((x) => clamp(x, 0, 1), [2], { clamp: (a) => a }), /not JSUI's expression helper/);
  const node = { __getNativeTag: () => 7 };
  const p = compileExpression('(x) => x + speed', 1, { speed: node });
  assert.deepEqual(p.inputs, [node]);
  assert.equal(evaluateExpression(p.code, [1, 2], p.slots), 3);
});

test('unsupported syntax is rejected with a message', () => {
  for (const src of ['(a) => [a].length', '(a) => foo(a)', '(a) => { a = 2; return a; }', '(a = 1) => a', '(a) => { return a; a; }', '(a) => Math.random()', 'async (a) => a', '({ a }) => a'])
    assert.throws(() => compileExpression(src, 1), Error, src);
});

test('NaN results become 0', () => {
  assert.equal(run((a) => a / 0 - a / 0, [1]), 0);
  assert.equal(run((a) => Math.sqrt(a), [-1]), 0);
});

test('parseExpressionFunction finds the end and free names', () => {
  const src = 'derive([a], (x, y) => x * k + clamp(y, 0, 1) + Expr.lerp(0, 1, x), 5)';
  const at = src.indexOf('(x');
  const r = parseExpressionFunction(src, at);
  assert.equal(src.slice(r.end, r.end + 3), ', 5');
  assert.deepEqual(r.free.sort(), ['Expr', 'clamp', 'k']);
});

test('variadic hypot', () => {
  assert.ok(Math.abs(run((a, b) => Math.hypot(a, b, 2), [3, 4]) - Math.hypot(3, 4, 2)) < 1e-12);
});
