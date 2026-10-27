// React-free checks for the fetch() family that need no network: Streams,
// Blob / File, FormData (multipart both ways), Request / Response bodies.
// Run by fetch_test.cpp; reports __jsui_native.postMessage('smoke', {passed,
// failures}) once every async check has finished.
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
  const later = (name, p) => pending.push(Promise.resolve(p).catch((e) => failures.push(name + ' threw ' + e + (e && e.stack ? ' ' + e.stack : ''))));
  const enc = new TextEncoder(), dec = new TextDecoder();

  for (const g of ['ReadableStream', 'WritableStream', 'TransformStream', 'TextDecoderStream', 'TextEncoderStream',
    'Blob', 'File', 'FormData', 'ReadableStreamDefaultReader', 'CountQueuingStrategy', 'ByteLengthQueuingStrategy'])
    check('global ' + g, typeof globalThis[g] === 'function');

  // ---- ReadableStream: reader, pull, cancel, errors, async iteration, tee
  later('stream reader', (async () => {
    let n = 0;
    const s = new ReadableStream({ pull(c) { if (n < 3) c.enqueue(n++); else c.close(); } });
    check('locked before', !s.locked);
    const r = s.getReader();
    check('locked after', s.locked);
    const got = [];
    for (;;) { const { value, done } = await r.read(); if (done) break; got.push(value); }
    eq('pull values', got.join(','), '0,1,2');
    await r.closed;
    r.releaseLock();
    check('unlocked', !s.locked);
  })());
  later('stream async iterate', (async () => {
    const s = ReadableStream.from(['a', 'b', 'c']);
    let out = '';
    for await (const v of s) out += v;
    eq('for await', out, 'abc');
  })());
  later('stream cancel', (async () => {
    let reason = null;
    const s = new ReadableStream({ start(c) { c.enqueue(1); }, cancel(r) { reason = r; } });
    await s.cancel('bye');
    eq('cancel reason', reason, 'bye');
    const { done } = await s.getReader().read();
    check('cancelled reads done', done);
  })());
  later('stream error', (async () => {
    const s = new ReadableStream({ start(c) { c.error(new TypeError('boom')); } });
    try { await s.getReader().read(); failures.push('error: should reject'); } catch (e) { eq('error message', e.message, 'boom'); }
  })());
  later('stream tee', (async () => {
    const [a, b] = ReadableStream.from([1, 2, 3]).tee();
    const all = async (s) => { const out = []; for await (const v of s) out.push(v); return out.join(','); };
    const [x, y] = await Promise.all([all(a), all(b)]);
    eq('tee a', x, '1,2,3');
    eq('tee b', y, '1,2,3');
  })());
  later('stream pipe', (async () => {
    const chunks = [];
    const ws = new WritableStream({ write(c) { chunks.push(c); } });
    await ReadableStream.from([enc.encode('hé'), enc.encode('llo')]).pipeThrough(new TextDecoderStream()).pipeTo(ws);
    eq('pipe text', chunks.join(''), 'héllo');
    // A multi-byte character split across chunks.
    const bytes = enc.encode('€');
    const parts = [];
    const ts = ReadableStream.from([bytes.slice(0, 1), bytes.slice(1)]).pipeThrough(new TextDecoderStream());
    for await (const p of ts) parts.push(p);
    eq('split utf-8', parts.join(''), '€');
    const up = [];
    for await (const p of ReadableStream.from(['x', 'yz']).pipeThrough(new TransformStream({
      transform(c, ctl) { ctl.enqueue(c.toUpperCase()); },
      flush(ctl) { ctl.enqueue('!'); },
    }))) up.push(p);
    eq('transform', up.join(''), 'XYZ!');
  })());

  // ---- Blob / File
  later('blob', (async () => {
    const b = new Blob(['ab', enc.encode('cd'), new Blob(['ef'])], { type: 'Text/Plain' });
    eq('blob size', b.size, 6);
    eq('blob type', b.type, 'text/plain');
    eq('blob text', await b.text(), 'abcdef');
    eq('blob slice', await b.slice(1, -1).text(), 'bcde');
    eq('blob slice neg', await b.slice(-2).text(), 'ef');
    eq('blob bytes', (await b.bytes()).length, 6);
    eq('blob arrayBuffer', (await b.arrayBuffer()).byteLength, 6);
    let s = '';
    for await (const c of b.stream()) s += dec.decode(c);
    eq('blob stream', s, 'abcdef');
    const f = new File(['x'], 'a.txt', { type: 'text/plain', lastModified: 5 });
    check('file is blob', f instanceof Blob);
    eq('file name', f.name, 'a.txt');
    eq('file lastModified', f.lastModified, 5);
  })());

  // ---- FormData + multipart roundtrip through Response
  later('formdata', (async () => {
    const fd = new FormData();
    fd.append('a', '1');
    fd.append('a', '2');
    fd.append('file', new Blob([new Uint8Array([0, 1, 255])], { type: 'application/x-bin' }), 'data.bin');
    fd.set('b', 'line1\nline2 "q"');
    eq('fd getAll', fd.getAll('a').join(','), '1,2');
    check('fd file', fd.get('file') instanceof File && fd.get('file').name === 'data.bin');
    const res = new Response(fd);
    const type = res.headers.get('content-type');
    check('multipart type', /^multipart\/form-data; boundary=/.test(type), type);
    const back = await res.formData();
    eq('fd back a', back.getAll('a').join(','), '1,2');
    eq('fd back b', back.get('b'), 'line1\r\nline2 "q"');
    const file = back.get('file');
    check('fd back file', file instanceof File, String(file));
    eq('fd back filename', file.name, 'data.bin');
    eq('fd back file type', file.type, 'application/x-bin');
    eq('fd back bytes', Array.from(await file.bytes()).join(','), '0,1,255');
    const url = await new Response('x=1&y=%C3%A9', { headers: { 'content-type': 'application/x-www-form-urlencoded' } }).formData();
    eq('urlencoded', url.get('y'), 'é');
    const fd2 = new FormData();
    fd2.append('k', 'v');
    fd2.delete('k');
    check('fd delete', !fd2.has('k'));
  })());

  // ---- Response / Request bodies: streams, blobs, clone, bodyUsed
  later('bodies', (async () => {
    const r = new Response(ReadableStream.from([enc.encode('str'), enc.encode('eam')]));
    const c = r.clone();
    eq('stream body', await r.text(), 'stream');
    eq('cloned stream body', await c.text(), 'stream');
    check('bodyUsed', r.bodyUsed);
    try { await r.text(); failures.push('second read should reject'); } catch (e) { check('double read TypeError', e instanceof TypeError); }
    const r2 = new Response('hello');
    const reader = r2.body.getReader();
    const first = await reader.read();
    eq('body reader', dec.decode(first.value), 'hello');
    check('bodyUsed after read', r2.bodyUsed);
    const blob = await new Response(new Blob(['bl'], { type: 'x/y' })).blob();
    eq('blob body type', blob.type, 'x/y');
    eq('blob body text', await blob.text(), 'bl');
    eq('null body', new Response(null).body, null);
    const req = new Request('http://h/x', { method: 'POST', body: new Blob(['up']) });
    eq('request body text', await req.clone().text(), 'up');
    const fdReq = new Request('http://h/x', { method: 'POST', body: new FormData() });
    check('request multipart type', /^multipart\/form-data/.test(fdReq.headers.get('content-type')));
    let threw = false;
    try { new Request('http://h/', { redirect: 'nope' }); } catch (e) { threw = e instanceof TypeError; }
    check('bad redirect throws', threw);
  })());

  // ---- fetch over data: as a stream
  later('fetch stream', (async () => {
    const res = await fetch('data:text/plain,streamed%20data');
    const reader = res.body.getReader();
    let s = '';
    for (;;) { const { value, done } = await reader.read(); if (done) break; s += dec.decode(value, { stream: true }); }
    eq('data stream', s, 'streamed data');
  })());

  setTimeout(() => {
    Promise.all(pending).then(() => {
      __jsui_native.postMessage('smoke', JSON.stringify({ passed, failures }));
    });
  }, 20);
})();
