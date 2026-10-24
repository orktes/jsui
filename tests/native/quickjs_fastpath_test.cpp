// Spec behaviour of the QuickJS fast paths patched in by cmake/Deps.cmake.
// Each script runs in a fresh runtime and throws on the first mismatch. The
// cases target where a fast path could diverge from the slow path: getters
// and setters, proxies, prototype changes, frozen / sealed objects, deleted
// properties, integer-like keys, symbols and mixed-type comparisons.
#include <cstring>
#include <string>

#include "quickjs.h"
#include "test.h"

namespace {

// Runs `src` as a global script; returns "" or the exception text.
std::string runScript(const char* src) {
  JSRuntime* rt = JS_NewRuntime();
  JSContext* ctx = JS_NewContext(rt);
  std::string err;
  JSValue r = JS_Eval(ctx, src, std::strlen(src), "fastpath.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) {
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    err = s ? s : "(exception)";
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    if (const char* st = JS_ToCString(ctx, stack)) {
      err += "\n";
      err += st;
      JS_FreeCString(ctx, st);
    }
    JS_FreeValue(ctx, stack);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
  }
  JS_FreeValue(ctx, r);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return err;
}

#define JS_CASE(name, src)                                                       \
  TEST(name) {                                                                   \
    std::string err = runScript((std::string(kPrelude) + src).c_str());                               \
    if (!err.empty()) std::fprintf(stderr, "  %s: %s\n", #name, err.c_str());    \
    CHECK(err.empty());                                                          \
  }

const char kPrelude[] =
    "function eq(a, b, what) {"
    "  if (!Object.is(a, b)) throw new Error((what || 'value') + ': ' + String(a) + ' !== ' + String(b));"
    "}"
    "function same(a, b, what) { eq(JSON.stringify(a), JSON.stringify(b), what); }"
    "function throws(f, what) {"
    "  try { f(); } catch (e) { return e; }"
    "  throw new Error((what || 'call') + ' did not throw');"
    "}"
    // Runs f enough times to warm any per-site cache, with a fresh i each time.
    "function loop(f) { for (let i = 0; i < 50; i++) f(i); }\n";

}  // namespace

JS_CASE(qjs_strict_eq, R"JS(
  const s1 = 'ab' + 'c', s2 = ['a', 'bc'].join('');
  loop(() => {
    eq(s1 === s2, true, 'equal strings, different JSString');
    eq(s1 !== s2, false, 'equal strings !==');
    eq('abc' === 'abd', false, 'different strings');
    eq('' === '', true, 'empty strings');
    eq('é' === 'é', false, 'wide vs other');
    eq('éx' === ('é' + 'x'), true, 'latin1 strings');
    eq('中' === '中', true, 'wide strings');
    eq(1 === 1.0, true, 'int vs float');
    eq(0 === -0, true, '+0 === -0');
    eq(NaN === NaN, false, 'NaN');
    eq(NaN !== NaN, true, 'NaN !==');
    eq(2 ** 31 === 2147483648, true, 'float ints');
    eq(1 === '1', false, 'number vs string');
    eq('1' !== 1, true, 'string vs number !==');
    eq({} === '[object Object]', false, 'object vs string');
    eq(1n === 1n, true, 'short bigint');
    eq(2n ** 70n === 2n ** 70n, true, 'bigint');
    eq(1n === 1, false, 'bigint vs number');
    eq(true === 1, false, 'bool vs int');
    eq(null === undefined, false, 'null vs undefined');
    const sym = Symbol('x');
    eq(sym === sym, true, 'symbol');
    eq(sym === Symbol('x'), false, 'symbols');
    eq(Symbol.for('k') === Symbol.for('k'), true, 'registered symbols');
    const o = {};
    eq(o === o, true, 'object');
    eq(o === {}, false, 'objects');
    // Ropes (long concatenations) against flat strings.
    let r = ''; for (let k = 0; k < 300; k++) r += 'xy';
    eq(r === 'xy'.repeat(300), true, 'rope vs flat');
    eq(r === 'xy'.repeat(299) + 'xz', false, 'rope vs different flat');
    eq(r !== 1, true, 'rope vs number');
  });
)JS")

JS_CASE(qjs_object_keys, R"JS(
  loop(() => {
    same(Object.keys({b: 1, a: 2, c: 3}), ['b', 'a', 'c'], 'insertion order');
    same(Object.keys({b: 1, 2: 'x', a: 2, 1: 'y'}), ['1', '2', 'b', 'a'], 'integer keys first');
    same(Object.keys({4294967294: 1, 4294967295: 2, a: 3}), ['4294967294', '4294967295', 'a'], 'large index');
    same(Object.keys({['-1']: 1, '01': 2, 3: 4}), ['3', '-1', '01'], 'non-canonical numbers');
    const sym = Symbol('s');
    same(Object.keys({a: 1, [sym]: 2, b: 3}), ['a', 'b'], 'symbols skipped');
    const o = {a: 1, b: 2, c: 3};
    delete o.b;
    same(Object.keys(o), ['a', 'c'], 'deleted key');
    Object.defineProperty(o, 'hidden', {value: 1, enumerable: false});
    Object.defineProperty(o, 'g', {get() { throw new Error('getter ran'); }, enumerable: true});
    same(Object.keys(o), ['a', 'c', 'g'], 'non-enumerable skipped, getter not run');
    same(Object.keys(Object.freeze({x: 1, y: 2})), ['x', 'y'], 'frozen');
    same(Object.keys(Object.create({inherited: 1}, {own: {value: 1, enumerable: true}})), ['own'], 'own only');
    same(Object.keys([5, 6, 7]), ['0', '1', '2'], 'array');
    same(Object.keys('ab'), ['0', '1'], 'string primitive');
    same(Object.keys(new String('ab')), ['0', '1'], 'string object');
    same(Object.keys(new Uint8Array(2)), ['0', '1'], 'typed array');
    same(Object.keys(function f(a) {}), [], 'function');
    same(Object.keys(class { static m() {} static p = 1; }), ['p'], 'class');
    same(Object.keys(new (class { #priv = 1; pub = 2; })()), ['pub'], 'private field');
    const log = [];
    const p = new Proxy({a: 1, b: 2}, {
      ownKeys(t) { log.push('ownKeys'); return ['b', 'a']; },
      getOwnPropertyDescriptor(t, k) { log.push('gopd ' + k); return Reflect.getOwnPropertyDescriptor(t, k); },
    });
    same(Object.keys(p), ['b', 'a'], 'proxy keys');
    same(log, ['ownKeys', 'gopd b', 'gopd a'], 'proxy traps');
    eq(throws(() => Object.keys(null)) instanceof TypeError, true, 'null');
    eq(throws(() => Object.keys()) instanceof TypeError, true, 'no argument');
    same(Object.keys(1), [], 'number');
    const args = (function () { return arguments; })(1, 2);
    same(Object.keys(args), ['0', '1'], 'arguments');
    const k = Object.keys({a: 1});
    k.push('b');
    same(k, ['a', 'b'], 'result is a normal array');
    same(Object.values({a: 1, b: 2}), [1, 2], 'values');
    same(Object.entries({a: 1, 2: 3}), [['2', 3], ['a', 1]], 'entries');
  });
  eq(Object.keys(globalThis).includes('eq'), true, 'global function declarations are enumerable');
  globalThis.enumerableGlobal = 1;
  eq(Object.keys(globalThis).includes('enumerableGlobal'), true, 'global keys');
)JS")

JS_CASE(qjs_object_literals, R"JS(
  loop((i) => {
    const o = {a: i, b: 'x', c: null};
    same(Object.keys(o), ['a', 'b', 'c'], 'keys');
    eq(o.a, i, 'value');
    const d = Object.getOwnPropertyDescriptor(o, 'b');
    same(d, {value: 'x', writable: true, enumerable: true, configurable: true}, 'descriptor');
    const dup = {a: 1, b: 2, a: 3};
    same(Object.keys(dup), ['a', 'b'], 'duplicate key keeps first position');
    eq(dup.a, 3, 'duplicate key keeps last value');
    // A setter on the prototype is not called by a literal field.
    let called = false;
    Object.defineProperty(Object.prototype, 'trap', {set(v) { called = true; }, configurable: true});
    const t = {trap: 1};
    delete Object.prototype.trap;
    eq(called, false, 'literal field does not call inherited setter');
    eq(Object.getOwnPropertyDescriptor(t, 'trap').value, 1, 'own data property');
    const withProto = {__proto__: {inherited: 1}, own: 2};
    eq(withProto.inherited, 1, '__proto__ literal');
    same(Object.keys(withProto), ['own'], '__proto__ is not a key');
    const spread = {...{a: 1, b: 2}, b: 3, c: 4};
    same(spread, {a: 1, b: 3, c: 4}, 'spread then fields');
    const computed = {['k' + i]: i, 0: 'zero', [Symbol.iterator]: null};
    same(Object.keys(computed), ['0', 'k' + i], 'computed and integer keys');
    const getter = {get g() { return 7; }, v: 1};
    eq(getter.g, 7, 'getter in literal');
    const arr = [1, 2, 3];
    eq(arr.length, 3, 'array literal');
  });
)JS")

JS_CASE(qjs_own_property_names, R"JS(
  loop(() => {
    const sym = Symbol('s');
    const o = {b: 1, a: 2, [sym]: 3, 10: 'x', 2: 'y'};
    Object.defineProperty(o, 'hidden', {value: 1, enumerable: false});
    same(Object.getOwnPropertyNames(o), ['2', '10', 'b', 'a', 'hidden'], 'getOwnPropertyNames');
    eq(Reflect.ownKeys(o).length, 6, 'Reflect.ownKeys includes the symbol');
    eq(Reflect.ownKeys(o)[5], sym, 'symbol last');
    same(Object.getOwnPropertySymbols(o).length, 1, 'symbols');
    const plain = {z: 1, y: 2};
    Object.defineProperty(plain, 'h', {value: 3, enumerable: false});
    same(Object.getOwnPropertyNames(plain), ['z', 'y', 'h'], 'strings only');
    same(JSON.stringify(plain), '{"z":1,"y":2}', 'JSON.stringify');
    same(Object.assign({}, plain, {x: 0}), {z: 1, y: 2, x: 0}, 'Object.assign');
    same({...plain}, {z: 1, y: 2}, 'spread');
    same(Object.entries(Object.getOwnPropertyDescriptors(plain)).map(e => e[0]), ['z', 'y', 'h'], 'descriptors');
    const f = Object.freeze({q: 1});
    same(Object.getOwnPropertyNames(f), ['q'], 'frozen');
    eq(Object.isFrozen(f), true, 'isFrozen');
    same(Object.getOwnPropertyNames([1, 2]), ['0', '1', 'length'], 'array');
  });
)JS")

JS_CASE(qjs_for_in, R"JS(
  function keys(o) { const r = []; for (const k in o) r.push(k); return r; }
  loop(() => {
    same(keys({b: 1, a: 2, 1: 3}), ['1', 'b', 'a'], 'own keys in order');
    same(keys(Object.create({inherited: 1, shared: 2}, {own: {value: 1, enumerable: true}, shared: {value: 1, enumerable: false}})),
         ['own', 'inherited'], 'inherited keys after own, shadowed non-enumerable hides');
    same(keys([7, 8]), ['0', '1'], 'array');
    const arr = [1, 2]; arr.extra = 3;
    same(keys(arr), ['0', '1', 'extra'], 'array with extra key');
    same(keys('hi'), ['0', '1'], 'string');
    same(keys(null), [], 'null');
    same(keys(undefined), [], 'undefined');
    same(keys(42), [], 'number');
    const sym = Symbol();
    same(keys({[sym]: 1, s: 2}), ['s'], 'symbols skipped');
    // Deleting a key not yet visited skips it; adding keys does not visit them.
    const o = {a: 1, b: 2, c: 3};
    const seen = [];
    for (const k in o) { seen.push(k); if (k === 'a') { delete o.b; o.d = 4; } }
    same(seen, ['a', 'c'], 'delete during iteration');
    // A key deleted and re-added before it is reached is still visited once.
    const o2 = {a: 1, b: 2};
    const seen2 = [];
    for (const k in o2) { seen2.push(k); if (k === 'a') { delete o2.b; o2.b = 3; } }
    same(seen2, ['a', 'b'], 'delete and re-add');
    // An enumerable key added to the prototype is seen.
    const proto = {};
    const child = Object.create(proto);
    child.own = 1;
    proto.later = 2;
    same(keys(child), ['own', 'later'], 'prototype keys');
    // Proxy: ownKeys and getOwnPropertyDescriptor traps.
    const log = [];
    const p = new Proxy({x: 1, y: 2}, {
      ownKeys(t) { log.push('ownKeys'); return Reflect.ownKeys(t); },
      getOwnPropertyDescriptor(t, k) { log.push('gopd ' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
    });
    same(keys(p), ['x', 'y'], 'proxy keys');
    eq(log[0], 'ownKeys', 'proxy ownKeys trap');
    // Proxy as a prototype.
    const pp = Object.create(new Proxy({fromProxy: 1}, {}));
    pp.own = 1;
    same(keys(pp), ['own', 'fromProxy'], 'proxy prototype');
    // Prototype change during iteration does not change the visited keys.
    const a = {k1: 1};
    const b = Object.create({p1: 1});
    b.k2 = 2;
    const seen3 = [];
    for (const k in b) { seen3.push(k); Object.setPrototypeOf(b, a); }
    eq(seen3[0], 'k2', 'own first');
    // Getters are not called by for-in.
    const g = {get boom() { throw new Error('getter called'); }};
    same(keys(g), ['boom'], 'getter key');
    same(keys(Object.freeze({f: 1})), ['f'], 'frozen');
    same(keys(new (class { constructor() { this.x = 1; } m() {} })()), ['x'], 'class methods are not enumerable');
    // for-in over the arguments object and typed arrays.
    same((function () { return keys(arguments); })(1, 2), ['0', '1'], 'arguments');
    same(keys(new Int8Array(2)), ['0', '1'], 'typed array');
    // break and continue leave no iterator behind.
    let n = 0;
    for (const k in {a: 1, b: 2, c: 3}) { if (k === 'b') break; n++; }
    eq(n, 1, 'break');
  });
  // Enumerable properties on Object.prototype are seen by every for-in.
  Object.prototype.polluted = 1;
  try {
    same(keys({own: 1}), ['own', 'polluted'], 'Object.prototype key');
  } finally {
    delete Object.prototype.polluted;
  }
  same(keys({own: 1}), ['own'], 'after delete');
)JS")

JS_CASE(qjs_shapes, R"JS(
  function Point(x, y) { this.x = x; this.y = y; this.z = 0; }
  const made = [];
  loop((i) => {
    const a = {k1: i, k2: 2, k3: 3};
    const b = {k1: i, k2: 2, k3: 3};
    made.push(a, b);
    // Changing one object of a shared shape does not touch the others.
    delete a.k2;
    same(Object.keys(a), ['k1', 'k3'], 'delete on one');
    same(Object.keys(b), ['k1', 'k2', 'k3'], 'other unchanged');
    Object.defineProperty(b, 'k1', {enumerable: false});
    same(Object.keys(b), ['k2', 'k3'], 'flags on one');
    const c = {k1: 0, k2: 0, k3: 0};
    same(Object.keys(c), ['k1', 'k2', 'k3'], 'new object after changes');
    same(Object.getOwnPropertyDescriptor(c, 'k1'), {value: 0, writable: true, enumerable: true, configurable: true}, 'fresh descriptor');
    Object.freeze(c);
    const d = {k1: 1, k2: 1, k3: 1};
    d.k1 = 5;
    eq(d.k1, 5, 'frozen sibling does not freeze others');
    eq(Object.isFrozen(d), false, 'not frozen');
    Object.setPrototypeOf(d, {inherited: 1});
    eq(d.inherited, 1, 'prototype change on one');
    eq(({k1: 1, k2: 1, k3: 1}).inherited, undefined, 'prototype change does not leak');
    const p = new Point(i, 1);
    const q = new Point(2, i);
    eq(p.x + q.y, 2 * i, 'constructor fields');
    same(Object.keys(q), ['x', 'y', 'z'], 'constructor order');
    q.w = 1;
    same(Object.keys(new Point(0, 0)), ['x', 'y', 'z'], 'extra field stays on its object');
    // A setter or read-only property on the prototype still wins over adding.
    let set = 0;
    function WithSetter() { this.s = 1; }
    Object.defineProperty(WithSetter.prototype, 's', {set(v) { set += v; }, configurable: true});
    new WithSetter();
    eq(set, 1, 'prototype setter called by this.s = ...');
    function ReadOnly() { 'use strict'; this.r = 1; }
    Object.defineProperty(ReadOnly.prototype, 'r', {value: 0, writable: false});
    eq(throws(() => new ReadOnly()) instanceof TypeError, true, 'read-only prototype property throws in strict code');
    function Sloppy() { this.r = 1; }
    Sloppy.prototype = ReadOnly.prototype;
    eq(Object.hasOwn(new Sloppy(), 'r'), false, 'read-only prototype property is not shadowed');
    // Non-extensible and proxy receivers.
    const ne = Object.preventExtensions({});
    eq(throws(() => { 'use strict'; ne.x = 1; }) instanceof TypeError, true, 'non-extensible');
    const log = [];
    const px = new Proxy({}, {set(t, k, v) { log.push(k); t[k] = v; return true; }});
    px.a = 1;
    same(log, ['a'], 'proxy set trap');
    // Many keys (dictionary-like) and many distinct literal shapes.
    const dict = {};
    for (let k = 0; k < 100; k++) dict['key' + k] = k;
    eq(Object.keys(dict).length, 100, 'dictionary');
    eq(dict.key99, 99, 'dictionary value');
    const wide = {};
    for (let k = 0; k < 40; k++) wide['w' + k] = k;
    for (let k = 0; k < 40; k += 2) delete wide['w' + k];
    same(Object.keys(wide).slice(0, 3), ['w1', 'w3', 'w5'], 'deletes in a big object');
  });
  eq(made.length, 100, 'objects kept alive');
  eq(made[98].k1, 49, 'values of kept objects');
)JS")
