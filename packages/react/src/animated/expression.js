// Animated.derive expressions: a small pure-math subset of JavaScript that
// compiles to a stack program the runtime evaluates natively every frame
// (src/animated/native_animated.cpp, NativeAnimated "expression" nodes), so
// values derived from Animated values, host buffers and the clock need no JS
// and no frame of lag.
//
//   Animated.derive([hp, maxHp], (h, m) => clamp(h / m, 0, 1))
//   Animated.derive([x, cam], (x, c) => {
//     const s = (x - c) * 0.5;
//     if (s < 0) return 0;
//     return Math.min(s, 400);
//   })
//
// Accepted: number literals, true / false, the parameters, `const` / `let` /
// `var` locals, + - * / % **, unary - + !, < <= > >= == === != !==, && || ?:,
// `if (c) return a; [else] ...`, Math.* functions and constants, and the
// helpers below (bare or as X.clamp). Everything is a number (true is 1);
// NaN results become 0. Other free identifiers are captured by `jsui build`
// (see packages/cli/src/derive.js): numbers become constants and Animated
// nodes become extra inputs.
//
// The same parser runs at build time (to find the function's extent and
// free names) and at run time on the source text.

// Op codes; native_animated.cpp mirrors them.
export const OP = {
  CONST: 0, INPUT: 1, LOAD: 2, STORE: 3,
  ADD: 10, SUB: 11, MUL: 12, DIV: 13, MOD: 14, POW: 15, NEG: 16, NOT: 17,
  LT: 20, LE: 21, GT: 22, GE: 23, EQ: 24, NE: 25, AND: 26, OR: 27, SELECT: 28,
  CALL: 40,
};

// Functions (name, arity) by id; native_animated.cpp mirrors the order.
export const FUNCS = [
  ['abs', 1], ['sign', 1], ['floor', 1], ['ceil', 1], ['round', 1], ['trunc', 1],
  ['sqrt', 1], ['cbrt', 1], ['exp', 1], ['log', 1], ['log2', 1], ['log10', 1],
  ['sin', 1], ['cos', 1], ['tan', 1], ['asin', 1], ['acos', 1], ['atan', 1], ['atan2', 2],
  ['sinh', 1], ['cosh', 1], ['tanh', 1], ['pow', 2], ['hypot', 2], ['min', 2], ['max', 2],
  ['clamp', 3], ['lerp', 3], ['step', 2], ['smoothstep', 3], ['fract', 1], ['wrap', 2],
];
const FUNC_ID = new Map(FUNCS.map(([n], i) => [n, i]));

// GLSL-style helpers, usable in expressions and in plain JS.
export const clamp = (x, lo, hi) => Math.min(Math.max(x, lo), hi);
export const lerp = (a, b, t) => a + (b - a) * t;
export const step = (edge, x) => (x < edge ? 0 : 1);
export const smoothstep = (e0, e1, x) => {
  const t = clamp((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
};
export const fract = (x) => x - Math.floor(x);
/** Positive modulo: wrap(-1, 360) === 359. */
export const wrap = (x, m) => ((x % m) + m) % m;
export const Expr = { clamp, lerp, mix: lerp, step, smoothstep, fract, wrap };
const HELPERS = new Map([
  ['clamp', clamp], ['lerp', lerp], ['mix', lerp], ['step', step], ['smoothstep', smoothstep], ['fract', fract], ['wrap', wrap],
]);
const MATH_FUNCS = new Set(['abs', 'sign', 'floor', 'ceil', 'round', 'trunc', 'sqrt', 'cbrt', 'exp', 'log', 'log2', 'log10',
  'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh', 'pow', 'hypot', 'min', 'max']);
const MATH_CONSTS = new Set(['PI', 'E', 'LN2', 'LN10', 'LOG2E', 'LOG10E', 'SQRT2', 'SQRT1_2']);
const GLOBAL_CONSTS = new Map([['Infinity', Infinity], ['NaN', NaN], ['undefined', NaN]]);

export class ExpressionError extends Error {}

// --- tokens ----------------------------------------------------------------------

const PUNCT = ['===', '!==', '**', '=>', '<=', '>=', '==', '!=', '&&', '||',
  '(', ')', '{', '}', ',', ';', '.', '?', ':', '+', '-', '*', '/', '%', '<', '>', '!', '='];

function isIdStart(c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_' || c === '$';
}
function isDigit(c) {
  return c >= '0' && c <= '9';
}

// Tokenizes src from `start`. Stops at the end of the text: the parser
// decides where the function ends and reports it.
function tokenize(src, start) {
  const out = [];
  let i = start;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r') {
      i++;
      continue;
    }
    if (c === '/' && src[i + 1] === '/') {
      while (i < n && src[i] !== '\n') i++;
      continue;
    }
    if (c === '/' && src[i + 1] === '*') {
      const e = src.indexOf('*/', i + 2);
      i = e < 0 ? n : e + 2;
      continue;
    }
    if (isDigit(c) || (c === '.' && isDigit(src[i + 1] || ''))) {
      const m = /^(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|(?:\d[\d_]*)?\.?[\d_]*(?:[eE][+-]?\d+)?)/.exec(src.slice(i, i + 64));
      const text = m[0];
      const clean = text.replace(/_/g, '');
      const v = /^0[xXbBoO]/.test(clean) ? Number(clean) : parseFloat(clean);
      out.push({ t: 'num', v, pos: i, end: i + text.length });
      i += text.length;
      continue;
    }
    if (isIdStart(c)) {
      let j = i + 1;
      while (j < n && (isIdStart(src[j]) || isDigit(src[j]))) j++;
      out.push({ t: 'id', v: src.slice(i, j), pos: i, end: j });
      i = j;
      continue;
    }
    const p = PUNCT.find((s) => src.startsWith(s, i));
    if (!p) {
      out.push({ t: 'bad', v: c, pos: i, end: i + 1 });
      return out;
    }
    out.push({ t: 'p', v: p, pos: i, end: i + p.length });
    i += p.length;
  }
  out.push({ t: 'eof', v: '', pos: n, end: n });
  return out;
}

// --- parser: source -> AST ----------------------------------------------------------
// AST nodes: {k:'num', v} | {k:'param', i} | {k:'local', name} | {k:'free', name}
// | {k:'bin', op, a, b} | {k:'un', op, a} | {k:'sel', c, a, b} | {k:'call', f, args}
// | {k:'let', name, value, body}

class Parser {
  constructor(src, start) {
    this.src = src;
    this.toks = tokenize(src, start);
    this.i = 0;
    this.params = [];
    this.free = new Set();
    this.scopes = [];  // arrays of local names, innermost last
  }
  peek(o = 0) {
    return this.toks[Math.min(this.i + o, this.toks.length - 1)];
  }
  next() {
    return this.toks[Math.min(this.i++, this.toks.length - 1)];
  }
  is(v, o = 0) {
    const t = this.peek(o);
    return (t.t === 'p' || t.t === 'id') && t.v === v;
  }
  eat(v) {
    if (this.is(v)) {
      this.i++;
      return true;
    }
    return false;
  }
  fail(msg, tok = this.peek()) {
    const line = this.src.slice(0, tok.pos).split('\n').length;
    throw new ExpressionError(`${msg} (expression line ${line}: "${this.src.slice(tok.pos, tok.pos + 24).split('\n')[0]}")`);
  }
  expect(v) {
    if (!this.eat(v)) this.fail(`expected '${v}'`);
  }
  ident() {
    const t = this.next();
    if (t.t !== 'id') this.fail('expected a name', t);
    return t.v;
  }
  skipType() {
    // TypeScript annotation `: number` (simple names only).
    if (this.eat(':')) this.ident();
  }

  parseFunction() {
    if (this.eat('async')) this.fail('async functions are not expressions');
    let block = false;
    if (this.eat('function')) {
      if (this.peek().t === 'id') this.next();
      this.paramList();
      this.skipType();
      block = true;
    } else if (this.peek().t === 'id' && this.is('=>', 1)) {
      this.params.push(this.ident());
      this.expect('=>');
    } else {
      this.paramList();
      this.skipType();
      this.expect('=>');
    }
    let body;
    if (block || this.is('{')) {
      this.expect('{');
      body = this.block();
    } else {
      body = this.expr();
    }
    return { params: this.params, body, end: this.peek().pos };
  }
  paramList() {
    this.expect('(');
    while (!this.is(')')) {
      const t = this.peek();
      if (t.t !== 'id') this.fail('parameters must be plain names');
      this.params.push(this.ident());
      this.skipType();
      if (this.is('=')) this.fail('default parameters are not supported');
      if (!this.eat(',')) break;
    }
    this.expect(')');
  }

  // Statements up to the closing '}' -> one AST that yields the return value.
  block() {
    this.scopes.push([]);
    const body = this.statements();
    this.expect('}');
    this.scopes.pop();
    return body;
  }
  statements() {
    while (this.eat(';'));
    if (this.is('}')) this.fail('the function must return a value');
    if (this.is('const') || this.is('let') || this.is('var')) {
      this.next();
      const decls = [];
      do {
        const name = this.ident();
        this.skipType();
        this.expect('=');
        decls.push([name, this.expr()]);
        this.scopes[this.scopes.length - 1].push(name);
      } while (this.eat(','));
      this.eat(';');
      let body = this.statements();
      for (let d = decls.length - 1; d >= 0; d--) body = { k: 'let', name: decls[d][0], value: decls[d][1], body };
      return body;
    }
    if (this.eat('return')) {
      const v = this.expr();
      this.eat(';');
      while (this.eat(';'));
      if (!this.is('}')) this.fail('code after return');
      return v;
    }
    if (this.eat('if')) {
      this.expect('(');
      const c = this.expr();
      this.expect(')');
      const a = this.branch();
      if (this.eat('else')) {
        // `else if` chains: the rest of the block is the else branch.
        if (this.is('if')) return { k: 'sel', c, a, b: this.statements() };
        const b = this.branch();
        while (this.eat(';'));
        if (!this.is('}')) this.fail('code after if / else that both return');
        return { k: 'sel', c, a, b };
      }
      return { k: 'sel', c, a, b: this.statements() };
    }
    this.fail('only const / let declarations, if and return are supported');
  }
  branch() {
    if (this.eat('{')) return this.block();
    if (this.is('if')) this.fail('put a nested if in braces');
    if (!this.eat('return')) this.fail('an if branch must return');
    const v = this.expr();
    this.eat(';');
    return v;
  }

  expr() {
    const c = this.or();
    if (this.eat('?')) {
      const a = this.expr();
      this.expect(':');
      const b = this.expr();
      return { k: 'sel', c, a, b };
    }
    return c;
  }
  or() {
    let a = this.and();
    while (this.eat('||')) a = { k: 'bin', op: OP.OR, a, b: this.and() };
    return a;
  }
  and() {
    let a = this.eq();
    while (this.eat('&&')) a = { k: 'bin', op: OP.AND, a, b: this.eq() };
    return a;
  }
  eq() {
    let a = this.rel();
    for (;;) {
      if (this.eat('===') || this.eat('==')) a = { k: 'bin', op: OP.EQ, a, b: this.rel() };
      else if (this.eat('!==') || this.eat('!=')) a = { k: 'bin', op: OP.NE, a, b: this.rel() };
      else return a;
    }
  }
  rel() {
    let a = this.add();
    for (;;) {
      if (this.eat('<=')) a = { k: 'bin', op: OP.LE, a, b: this.add() };
      else if (this.eat('>=')) a = { k: 'bin', op: OP.GE, a, b: this.add() };
      else if (this.eat('<')) a = { k: 'bin', op: OP.LT, a, b: this.add() };
      else if (this.eat('>')) a = { k: 'bin', op: OP.GT, a, b: this.add() };
      else return a;
    }
  }
  add() {
    let a = this.mul();
    for (;;) {
      if (this.eat('+')) a = { k: 'bin', op: OP.ADD, a, b: this.mul() };
      else if (this.eat('-')) a = { k: 'bin', op: OP.SUB, a, b: this.mul() };
      else return a;
    }
  }
  mul() {
    let a = this.unary();
    for (;;) {
      if (this.eat('*')) a = { k: 'bin', op: OP.MUL, a, b: this.unary() };
      else if (this.eat('/')) a = { k: 'bin', op: OP.DIV, a, b: this.unary() };
      else if (this.eat('%')) a = { k: 'bin', op: OP.MOD, a, b: this.unary() };
      else return a;
    }
  }
  unary() {
    if (this.eat('-')) return { k: 'un', op: OP.NEG, a: this.unary() };
    if (this.eat('+')) return this.unary();
    if (this.eat('!')) return { k: 'un', op: OP.NOT, a: this.unary() };
    if (this.eat('void')) {
      this.unary();
      return { k: 'num', v: NaN };
    }
    const a = this.postfix();
    if (this.eat('**')) return { k: 'bin', op: OP.POW, a, b: this.unary() };
    return a;
  }
  postfix() {
    const t = this.next();
    if (t.t === 'num') return { k: 'num', v: t.v };
    if (t.t === 'p' && t.v === '(') {
      const e = this.expr();
      if (this.is(',')) this.fail('comma expressions are not supported');
      this.expect(')');
      return e;
    }
    if (t.t !== 'id') this.fail(`unexpected '${t.v}'`, t);
    if (t.v === 'true') return { k: 'num', v: 1 };
    if (t.v === 'false') return { k: 'num', v: 0 };
    // X.name: Math.fn / Math.CONST / helpers through a namespace (Expr.clamp).
    let name = t.v, ns = null;
    if (this.eat('.')) {
      ns = name;
      name = this.ident();
      if (this.is('.')) this.fail('nested member access is not supported');
    }
    if (this.eat('(')) {
      const args = [];
      while (!this.is(')')) {
        args.push(this.expr());
        if (!this.eat(',')) break;
      }
      this.expect(')');
      return this.call(ns, name, args, t);
    }
    if (ns) {
      if (ns === 'Math' && MATH_CONSTS.has(name)) return { k: 'num', v: Math[name] };
      this.fail(`'${ns}.${name}' is not supported`, t);
    }
    const p = this.params.indexOf(name);
    for (let s = this.scopes.length - 1; s >= 0; s--) if (this.scopes[s].includes(name)) return { k: 'local', name };
    if (p >= 0) return { k: 'param', i: p };
    if (GLOBAL_CONSTS.has(name)) return { k: 'num', v: GLOBAL_CONSTS.get(name) };
    this.free.add(name);
    return { k: 'free', name };
  }
  call(ns, name, args, tok) {
    let fn = null;
    if (ns === 'Math') {
      if (!MATH_FUNCS.has(name)) this.fail(`Math.${name} is not supported`, tok);
      fn = name;
    } else {
      if (!HELPERS.has(name)) this.fail(`calls to '${ns ? ns + '.' : ''}${name}' are not supported`, tok);
      if (ns) this.free.add(ns);
      else this.free.add(name);
      fn = name === 'mix' ? 'lerp' : name;
      if (!ns) return { k: 'call', f: fn, args, helper: name };
    }
    if (fn === 'min' || fn === 'max') {
      if (args.length === 0) return { k: 'num', v: fn === 'min' ? Infinity : -Infinity };
      let a = args[0];
      for (let j = 1; j < args.length; j++) a = { k: 'call', f: fn, args: [a, args[j]] };
      return args.length === 1 ? { k: 'call', f: fn, args: [a, a] } : a;
    }
    if (fn === 'hypot' && args.length !== 2) {
      if (args.length === 0) return { k: 'num', v: 0 };
      // sqrt(a*a + b*b + ...)
      let s = null;
      for (const x of args) {
        const sq = { k: 'bin', op: OP.MUL, a: x, b: x };
        s = s ? { k: 'bin', op: OP.ADD, a: s, b: sq } : sq;
      }
      return { k: 'call', f: 'sqrt', args: [s] };
    }
    const arity = FUNCS[FUNC_ID.get(fn)][1];
    if (args.length !== arity) this.fail(`${fn} takes ${arity} argument${arity === 1 ? '' : 's'}`, tok);
    return { k: 'call', f: fn, args };
  }
}

/**
 * Parses the function that starts at `src[start]`. Returns its parameter
 * names, the free names it reads (to capture), the AST and the index just
 * after it. Throws ExpressionError for anything outside the subset.
 */
export function parseExpressionFunction(src, start = 0) {
  const p = new Parser(src, start);
  const fn = p.parseFunction();
  return { params: fn.params, free: [...p.free], ast: fn.body, end: fn.end };
}

// --- code generation -------------------------------------------------------------------

/**
 * Compiles `source` (an arrow function or function expression) for
 * `inputCount` inputs. `scope` maps captured free names to values: numbers
 * and booleans become constants, Animated nodes (anything with
 * __getNativeTag) become extra inputs. Returns {code, inputs: extra nodes,
 * slots, stack}.
 */
export function compileExpression(source, inputCount, scope) {
  const { params, ast } = parseExpressionFunction(source, 0);
  const code = [];
  const extra = [];
  const extraIndex = new Map();
  const locals = [];  // [{name, slot}] innermost last
  let slots = 0, depth = 0, maxDepth = 0;
  const push = (d) => {
    depth += d;
    if (depth > maxDepth) maxDepth = depth;
  };
  const scopeHas = (name) => scope && Object.prototype.hasOwnProperty.call(scope, name);
  const freeValue = (name) => {
    if (!scopeHas(name)) {
      throw new ExpressionError(
        `'${name}' is not defined in the expression: pass it as an input (Animated.derive([..., ${name}], ...)), or build with jsui build to capture it`,
      );
    }
    return scope[name];
  };
  const gen = (n) => {
    switch (n.k) {
      case 'num':
        code.push(OP.CONST, n.v);
        push(1);
        return;
      case 'param':
        if (n.i >= inputCount) throw new ExpressionError(`parameter '${params[n.i]}' has no input (${inputCount} given)`);
        code.push(OP.INPUT, n.i);
        push(1);
        return;
      case 'local': {
        for (let j = locals.length - 1; j >= 0; j--) {
          if (locals[j].name === n.name) {
            code.push(OP.LOAD, locals[j].slot);
            push(1);
            return;
          }
        }
        throw new ExpressionError(`'${n.name}' is used before it is defined`);
      }
      case 'free': {
        const v = freeValue(n.name);
        if (typeof v === 'number' || typeof v === 'boolean') {
          code.push(OP.CONST, +v);
        } else if (v && typeof v.__getNativeTag === 'function') {
          if (!extraIndex.has(v)) {
            extraIndex.set(v, inputCount + extra.length);
            extra.push(v);
          }
          code.push(OP.INPUT, extraIndex.get(v));
        } else {
          throw new ExpressionError(`'${n.name}' is ${v === null ? 'null' : typeof v}: only numbers, booleans and Animated nodes can be captured`);
        }
        push(1);
        return;
      }
      case 'un':
        gen(n.a);
        code.push(n.op);
        return;
      case 'bin':
        gen(n.a);
        gen(n.b);
        code.push(n.op);
        push(-1);
        return;
      case 'sel':
        gen(n.c);
        gen(n.a);
        gen(n.b);
        code.push(OP.SELECT);
        push(-2);
        return;
      case 'call': {
        if (n.helper && scopeHas(n.helper) && scope[n.helper] !== HELPERS.get(n.helper)) {
          throw new ExpressionError(`'${n.helper}' is not JSUI's expression helper: import { ${n.helper} } from 'react-native' or inline it`);
        }
        for (const a of n.args) gen(a);
        code.push(OP.CALL, FUNC_ID.get(n.f));
        push(1 - n.args.length);
        return;
      }
      case 'let': {
        const slot = slots++;
        gen(n.value);
        code.push(OP.STORE, slot);
        push(-1);
        locals.push({ name: n.name, slot });
        gen(n.body);
        locals.pop();
        return;
      }
    }
  };
  gen(ast);
  return { code, inputs: extra, slots, stack: maxDepth };
}

// --- evaluation (JS driver) -------------------------------------------------------------

const truthy = (v) => v !== 0 && v === v;
const FN_IMPL = FUNCS.map(([name]) => {
  if (HELPERS.has(name)) return HELPERS.get(name);
  if (name === 'round') return (x) => Math.floor(x + 0.5);
  return Math[name];
});

/** Runs a compiled program on `inputs` (numbers); NaN becomes 0, like native. */
export function evaluateExpression(code, inputs, slots) {
  const stack = [];
  const locals = slots ? new Array(slots) : null;
  for (let pc = 0; pc < code.length; ) {
    const op = code[pc++];
    let b, a;
    switch (op) {
      case OP.CONST: stack.push(code[pc++]); break;
      case OP.INPUT: { const v = inputs[code[pc++]]; stack.push(typeof v === 'number' ? v : +v); break; }
      case OP.LOAD: stack.push(locals[code[pc++]]); break;
      case OP.STORE: locals[code[pc++]] = stack.pop(); break;
      case OP.NEG: stack.push(-stack.pop()); break;
      case OP.NOT: stack.push(truthy(stack.pop()) ? 0 : 1); break;
      case OP.SELECT: b = stack.pop(); a = stack.pop(); stack.push(truthy(stack.pop()) ? a : b); break;
      case OP.CALL: {
        const f = FUNCS[code[pc++]];
        const args = stack.splice(stack.length - f[1], f[1]);
        stack.push(FN_IMPL[FUNC_ID.get(f[0])](...args));
        break;
      }
      default:
        b = stack.pop();
        a = stack.pop();
        switch (op) {
          case OP.ADD: stack.push(a + b); break;
          case OP.SUB: stack.push(a - b); break;
          case OP.MUL: stack.push(a * b); break;
          case OP.DIV: stack.push(a / b); break;
          case OP.MOD: stack.push(a % b); break;
          case OP.POW: stack.push(a ** b); break;
          case OP.LT: stack.push(a < b ? 1 : 0); break;
          case OP.LE: stack.push(a <= b ? 1 : 0); break;
          case OP.GT: stack.push(a > b ? 1 : 0); break;
          case OP.GE: stack.push(a >= b ? 1 : 0); break;
          case OP.EQ: stack.push(a === b ? 1 : 0); break;
          case OP.NE: stack.push(a !== b ? 1 : 0); break;
          case OP.AND: stack.push(truthy(a) ? b : a); break;
          case OP.OR: stack.push(truthy(a) ? a : b); break;
          default: throw new ExpressionError(`bad op ${op}`);
        }
    }
  }
  const v = stack.pop();
  return v === v ? v : 0;
}
