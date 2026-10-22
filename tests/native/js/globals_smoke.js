// React-free smoke bundle for the JS host globals. Run by host_test.cpp
// (and usable with `jsui-render tests/native/js/globals_smoke.js`). Reports
// through __jsui_native.postMessage('smoke', {passed, failures}) once all
// async checks have finished.
(function () {
  'use strict';
  const failures = [];
  let passed = 0;
  const check = (name, cond, detail) => {
    if (cond) passed++;
    else failures.push(name + (detail !== undefined ? ' (' + detail + ')' : ''));
  };
  const eq = (name, a, b) => check(name, a === b, JSON.stringify(a) + ' !== ' + JSON.stringify(b));
  const pending = [];
  const later = (name, p) => pending.push(Promise.resolve(p).catch((e) => failures.push(name + ' threw ' + e)));

  // ---- globals present
  for (const g of ['console', 'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'setImmediate',
    'requestAnimationFrame', 'cancelAnimationFrame', 'queueMicrotask', 'performance', 'fetch', 'Headers',
    'Request', 'Response', 'TextEncoder', 'TextDecoder', 'URL', 'URLSearchParams', 'MessageChannel',
    'AbortController', 'EventTarget', 'reportError', 'structuredClone', 'atob', 'btoa', 'DOMException'])
    check('global ' + g, typeof globalThis[g] !== 'undefined');
  check('window === globalThis', window === globalThis && self === globalThis);
  check('performance.now number', typeof performance.now() === 'number');
  check('location.href', typeof location.href === 'string' && location.href.length > 0, location.href);

  // ---- console formatting does not throw (output goes to the log)
  console.log('smoke: %s is %d years and %o', 'bob', 42.9, { a: [1, 2] }, 'extra');
  console.error('Warning: %s%s', 'Each child in a list should have a unique "key" prop.', '\n    in View');
  const cyc = { name: 'cyc' }; cyc.self = cyc;
  console.debug(cyc, new Map([[1, 'a']]), new Set([1]), new Error('logged error'), Symbol('s'), 10n);
  eq('inspect', __jsui_inspect({ a: 1, b: 'x', c: [1, 2] }), "{ a: 1, b: 'x', c: [ 1, 2 ] }");

  // ---- TextEncoder / TextDecoder
  const bytes = new TextEncoder().encode('hé€😀');
  eq('encode length', bytes.length, 1 + 2 + 3 + 4);
  eq('decode roundtrip', new TextDecoder().decode(bytes), 'hé€😀');
  eq('lone surrogate -> U+FFFD', new TextDecoder().decode(new TextEncoder().encode('a\ud800b')), 'a�b');
  eq('invalid -> U+FFFD', new TextDecoder().decode(new Uint8Array([0x61, 0xff, 0x62])), 'a�b');
  let threw = false;
  try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xc3])); } catch (e) { threw = e instanceof TypeError; }
  check('fatal decode throws', threw);
  const sd = new TextDecoder();
  eq('stream decode', sd.decode(bytes.subarray(0, 4), { stream: true }) + sd.decode(bytes.subarray(4)), 'hé€😀');
  eq('BOM stripped', new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])), 'A');

  // ---- URL / URLSearchParams
  const u = new URL('../img/a.png?x=1#frag', 'http://localhost:5173/app/sub/bundle.js');
  eq('url href', u.href, 'http://localhost:5173/app/img/a.png?x=1#frag');
  eq('url origin', u.origin, 'http://localhost:5173');
  eq('url host', u.host, 'localhost:5173');
  eq('url default port dropped', new URL('http://a.com:80/x').href, 'http://a.com/x');
  eq('url searchParams', u.searchParams.get('x'), '1');
  u.searchParams.append('y', 'a b&c');
  eq('url search update', u.search, '?x=1&y=a+b%26c');
  eq('url data', new URL('data:text/plain,hi').protocol, 'data:');
  eq('url file', new URL('file:///tmp/a/../b.txt').pathname, '/tmp/b.txt');
  const sp = new URLSearchParams({ q: 'jsui', n: 2 });
  eq('params toString', sp.toString(), 'q=jsui&n=2');
  eq('params parse', new URLSearchParams('?a=1&a=2&b=%20x').getAll('a').join(','), '1,2');
  threw = false;
  try { new URL('not a url'); } catch (e) { threw = e instanceof TypeError; }
  check('bad url throws', threw);

  // ---- Headers / Response
  const h = new Headers({ 'Content-Type': 'text/plain' });
  h.append('X-A', '1'); h.append('x-a', '2');
  eq('headers get', h.get('x-a'), '1, 2');
  eq('headers ci', h.get('CONTENT-TYPE'), 'text/plain');
  later('response json', new Response('{"k":[1]}', { status: 201 }).json().then((j) => {
    eq('response json value', j.k[0], 1);
  }));
  const r = new Response('body');
  check('response ok', r.ok && r.status === 200);

  // ---- structuredClone / AbortController / atob
  const orig = { d: new Date(5), m: new Map([[1, { x: 1 }]]), a: [1, { b: 2 }] };
  const cl = structuredClone(orig);
  check('structuredClone deep', cl !== orig && cl.m.get(1).x === 1 && cl.d.getTime() === 5 && cl.a[1].b === 2 && cl.a[1] !== orig.a[1]);
  const ac = new AbortController();
  let aborted = 0;
  ac.signal.addEventListener('abort', () => aborted++);
  ac.abort();
  check('abort event', aborted === 1 && ac.signal.aborted && ac.signal.reason.name === 'AbortError');
  eq('btoa/atob', atob(btoa('jsui')), 'jsui');

  // ---- event loop ordering: sync, microtasks, timers (by due time, then FIFO), MessageChannel
  const order = [];
  setTimeout(() => order.push('t10'), 10);
  setTimeout(() => order.push('t0a'), 0);
  setTimeout(() => order.push('t0b'), 0);
  setImmediate(() => order.push('imm'));
  const mc = new MessageChannel();
  mc.port1.onmessage = (e) => order.push('mc:' + e.data);
  mc.port2.postMessage('hi');
  Promise.resolve().then(() => order.push('p1')).then(() => order.push('p2'));
  queueMicrotask(() => order.push('qm'));
  const cancelled = setTimeout(() => order.push('cancelled'), 0);
  clearTimeout(cancelled);
  let ticks = 0;
  const iv = setInterval(() => { if (++ticks === 3) clearInterval(iv); }, 1);
  order.push('sync');

  // ---- rAF: timestamp, cancel, next-frame registration
  let rafTs = -1, rafCount = 0;
  const cancelledRaf = requestAnimationFrame(() => order.push('cancelled raf'));
  cancelAnimationFrame(cancelledRaf);
  requestAnimationFrame((ts) => { rafTs = ts; rafCount++; requestAnimationFrame(() => rafCount++); });

  // ---- fetch: data: URI, file (this bundle), missing file, abort
  later('fetch data', fetch('data:application/json;base64,eyJhIjoxfQ==').then((res) => {
    eq('data status', res.status, 200);
    eq('data content-type', res.headers.get('content-type'), 'application/json');
    return res.json();
  }).then((j) => eq('data json', j.a, 1)));
  later('fetch data text', fetch('data:,hello%20world').then((r2) => r2.text()).then((t) => eq('data pct', t, 'hello world')));
  later('fetch self', fetch('globals_smoke.js').then((res) => {
    check('file ok', res.ok, res.status);
    return res.text();
  }).then((t) => check('file content', t.indexOf('React-free smoke bundle') >= 0)));
  later('fetch missing', fetch('does-not-exist.json').then((res) => eq('missing status', res.status, 404)));
  later('fetch arrayBuffer', fetch('data:application/octet-stream;base64,AAEC/w==').then((res) => res.arrayBuffer()).then((b) => {
    const v = new Uint8Array(b);
    check('arrayBuffer bytes', v.length === 4 && v[3] === 255, Array.from(v).join(','));
  }));
  const ac2 = new AbortController();
  later('fetch abort', fetch('globals_smoke.js', { signal: ac2.signal }).then(
    () => failures.push('abort: should reject'),
    (e) => eq('abort reason', e.name, 'AbortError')));
  ac2.abort();

  // ---- unhandled rejection is reported (host test checks the error log)
  if (globalThis.__smokeRejection) Promise.reject(new Error('smoke unhandled rejection'));

  setTimeout(() => {
    eq('event order', order.join(','), 'sync,p1,qm,p2,t0a,t0b,imm,mc:hi,t10');
    check('interval ran 3x', ticks === 3, ticks);
    check('raf ran with timestamp', rafCount === 2 && rafTs >= 0, rafCount + ' ' + rafTs);
    Promise.all(pending).then(() => {
      __jsui_native.postMessage('smoke', JSON.stringify({ passed, failures }));
    });
  }, 50);
})();
