// The JS debugger end to end: `jsui dev` + a native runtime in dev mode
// (build/jsui-render --dev, built with JSUI_DEBUGGER) + a CDP client playing
// DevTools. A breakpoint set on a line of the app's JSX (through the source
// map, as DevTools does) stops the runtime there; scopes, evaluation,
// stepping, console and hot updates work through the relay. Skipped when
// build/jsui-render is missing; JSUI_RENDER names the binary (and makes it
// required).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dev } from '../src/index.js';
import { decodeMappings } from '../src/sourcemap.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..', '..');
const exe = process.platform === 'win32' ? 'jsui-render.exe' : 'jsui-render';
const binary = process.env.JSUI_RENDER || path.join(repo, 'build', exe);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const APP = `import React, { useEffect, useState } from 'react';
import { View, Text } from 'react-native';

export default function App() {
  const [n, setN] = useState(0);
  useEffect(() => {
    const t = setInterval(() => setN((v) => v + 1), 50);
    return () => clearInterval(t);
  }, []);
  const label = 'tick ' + n;
  console.log('rendered', { n });
  return (
    <View>
      <Text>{label}</Text>
    </View>
  );
}
`;
const LABEL_LINE = 9;  // 0-based: "const label = ..."

/** A CDP client over the target's WebSocket. */
async function connect(url) {
  const ws = new WebSocket(url);
  const events = [];
  const waiters = [];
  const replies = new Map();
  let nextId = 1;
  ws.addEventListener('message', (e) => {
    const msg = JSON.parse(e.data);
    if (msg.id && replies.has(msg.id)) {
      replies.get(msg.id)(msg);
      replies.delete(msg.id);
      return;
    }
    events.push(msg);
    for (const w of waiters.splice(0)) if (!w(msg)) waiters.push(w);
  });
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve);
    ws.addEventListener('error', () => reject(new Error('WebSocket failed: ' + url)));
  });
  return {
    ws,
    events,
    send(method, params = {}) {
      const id = nextId++;
      ws.send(JSON.stringify({ id, method, params }));
      return new Promise((resolve) => replies.set(id, resolve));
    },
    // The next event (after `after` events seen) matching pred.
    event(pred, after = 0, ms = 15000) {
      const hit = events.slice(after).find(pred);
      if (hit) return Promise.resolve(hit);
      return new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('timed out waiting for an event')), ms);
        waiters.push((m) => {
          if (!pred(m)) return false;
          clearTimeout(t);
          resolve(m);
          return true;
        });
      });
    },
  };
}

/** The generated line of `line` (0-based) in `source`, from a map. */
function generatedLine(map, source, line) {
  const si = map.sources.indexOf(source);
  assert.ok(si >= 0, `${source} is in the map (${map.sources.join(', ')})`);
  const lines = decodeMappings(map.mappings);
  for (let g = 0; g < lines.length; g++)
    for (const seg of lines[g]) if (seg.length >= 4 && seg[1] === si && seg[2] === line) return g;
  throw new Error(`no mapping for ${source}:${line}`);
}

function originalOf(map, line, column) {
  const segs = decodeMappings(map.mappings)[line] || [];
  let best = null;
  for (const seg of segs) if (seg.length >= 4 && seg[0] <= column) best = seg;
  return best && { source: map.sources[best[1]], line: best[2] };
}

test('debugger: breakpoints in JSX, scopes, stepping, console, hot updates (native runtime)', { skip: !process.env.JSUI_RENDER && !existsSync(binary) && `no ${binary}`, timeout: 90000 }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-debug-'));
  const appFile = path.join(dir, 'App.jsx');
  await writeFile(appFile, APP);
  await writeFile(path.join(dir, 'index.jsx'), "import { AppRegistry } from 'react-native';\nimport App from './App';\nAppRegistry.registerComponent('main', () => App);\n");
  const s = await dev({ entry: path.join(dir, 'index.jsx'), port: 0, log: () => {} });
  const origin = `http://127.0.0.1:${s.port}`;
  const child = spawn(binary, ['--dev', `${origin}/bundle.js`, '--wait-ms', '80000', '-o', path.join(dir, 'out.png')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', (d) => (output += d));
  child.stderr.on('data', (d) => (output += d));
  try {
    // The runtime shows up as a target, like a Node process would.
    let target = null;
    for (let i = 0; i < 300 && !target; i++) {
      const list = await (await fetch(`${origin}/json/list`)).json();
      target = list[0];
      if (!target) await sleep(50);
    }
    assert.ok(target, 'the runtime is listed in /json/list\n' + output);
    assert.match(target.webSocketDebuggerUrl, /^ws:\/\//);

    const c = await connect(target.webSocketDebuggerUrl);
    await c.send('Runtime.enable');
    await c.event((m) => m.method === 'Runtime.executionContextCreated');
    await c.send('Debugger.enable');
    const parsed = await c.event((m) => m.method === 'Debugger.scriptParsed' && m.params.url.endsWith('/bundle.js'));
    assert.ok(parsed.params.sourceMapURL, 'the bundle names its source map');
    const map = await (await fetch(new URL(parsed.params.sourceMapURL, parsed.params.url))).json();
    const source = pathToFileURL(appFile).href;
    const realSource = map.sources.find((x) => x.endsWith('/App.jsx')) || source;

    // Console calls arrive with their arguments as objects.
    const log = await c.event((m) => m.method === 'Runtime.consoleAPICalled');
    assert.equal(log.params.type, 'log');
    assert.equal(log.params.args[0].value, 'rendered');
    assert.equal(log.params.args[1].type, 'object');

    // A breakpoint on the JSX line, through the map (what DevTools does).
    const line = generatedLine(map, realSource, LABEL_LINE);
    const bp = await c.send('Debugger.setBreakpointByUrl', { urlRegex: '/bundle\\.js$', lineNumber: line, columnNumber: 0 });
    assert.ok(bp.result.breakpointId);
    const seen = c.events.length;
    const paused = await c.event((m) => m.method === 'Debugger.paused', seen);
    const top = paused.params.callFrames[0];
    assert.equal(top.functionName, 'App');
    assert.deepEqual(originalOf(map, top.location.lineNumber, top.location.columnNumber), { source: realSource, line: LABEL_LINE });

    // Scopes and evaluation in the paused frame.
    const props = await c.send('Runtime.getProperties', { objectId: top.scopeChain[0].object.objectId, ownProperties: true });
    const names = props.result.result.map((p) => p.name);
    assert.ok(names.includes('n'), 'the local scope has n: ' + names.join(','));
    const n = await c.send('Debugger.evaluateOnCallFrame', { callFrameId: top.callFrameId, expression: 'n * 10', returnByValue: true });
    assert.equal(typeof n.result.result.value, 'number');

    // Step over: the next line of the JSX.
    const before = c.events.length;
    await c.send('Debugger.stepOver');
    const stepped = await c.event((m) => m.method === 'Debugger.paused', before);
    const at = stepped.params.callFrames[0].location;
    assert.equal(originalOf(map, at.lineNumber, at.columnNumber).line, LABEL_LINE + 1);

    // Remove the breakpoint and resume: the app keeps rendering.
    await c.send('Debugger.removeBreakpoint', { breakpointId: bp.result.breakpointId });
    await c.send('Debugger.resume');
    await sleep(300);
    const later = await c.send('Runtime.evaluate', { expression: '1 + 1', returnByValue: true });
    assert.equal(later.result.result.value, 2);

    // With no breakpoints the statement traps are off; a pause request turns
    // them back on and stops the running app.
    const pauseSeen = c.events.length;
    await c.send('Debugger.pause');
    await c.event((m) => m.method === 'Debugger.paused', pauseSeen);
    await c.send('Debugger.resume');

    // A hot update is a script of its own, with an inline map to the JSX.
    const hotSeen = c.events.length;
    await writeFile(appFile, APP.replace("'tick '", "'tock '"));
    // (A rebuild that changed no module, e.g. from the watcher seeing the
    // files written just before startup, is an update with nothing to map.)
    const hot = await c.event((m) => m.method === 'Debugger.scriptParsed' && m.params.url.includes('/__jsui/hmr/') && m.params.sourceMapURL, hotSeen, 20000);
    assert.match(hot.params.sourceMapURL, /^data:application\/json/);

    // DevTools going away resumes the runtime and drops its breakpoints.
    c.ws.close();
  } finally {
    child.kill();
    await s.close();
  }
});

test('profiler: Profiler.start / stop records a CPU profile of the JSX (native runtime)', { skip: !process.env.JSUI_RENDER && !existsSync(binary) && `no ${binary}`, timeout: 60000 }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-profile-'));
  const appFile = path.join(dir, 'App.jsx');
  await writeFile(appFile, APP.replace("const label = 'tick ' + n;", "const label = 'tick ' + spin(n);") +
    'function spin(n) {\n  let s = 0;\n  for (let i = 0; i < 200000; i++) s += Math.sqrt(i + n);\n  return Math.round(s);\n}\n');
  await writeFile(path.join(dir, 'index.jsx'), "import { AppRegistry } from 'react-native';\nimport App from './App';\nAppRegistry.registerComponent('main', () => App);\n");
  const s = await dev({ entry: path.join(dir, 'index.jsx'), port: 0, log: () => {} });
  const origin = `http://127.0.0.1:${s.port}`;
  const child = spawn(binary, ['--dev', `${origin}/bundle.js`, '--wait-ms', '50000', '-o', path.join(dir, 'out.png')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', (d) => (output += d));
  child.stderr.on('data', (d) => (output += d));
  try {
    let target = null;
    for (let i = 0; i < 300 && !target; i++) {
      target = (await (await fetch(`${origin}/json/list`)).json())[0];
      if (!target) await sleep(50);
    }
    assert.ok(target, 'the runtime is listed in /json/list\n' + output);
    const c = await connect(target.webSocketDebuggerUrl);
    await c.send('Runtime.enable');
    await c.send('Debugger.enable');
    const parsed = await c.event((m) => m.method === 'Debugger.scriptParsed' && m.params.url.endsWith('/bundle.js'));
    await c.event((m) => m.method === 'Runtime.consoleAPICalled');  // the app renders

    assert.ok((await c.send('Profiler.enable')).result);
    assert.equal((await c.send('Profiler.stop')).error?.message, 'Profiling is not started');
    await c.send('Profiler.setSamplingInterval', { interval: 500 });
    assert.ok((await c.send('Profiler.start')).result);
    await sleep(1500);
    const { profile } = (await c.send('Profiler.stop')).result;

    assert.equal(profile.samples.length, profile.timeDeltas.length);
    assert.ok(profile.samples.length > 50, `${profile.samples.length} samples`);
    assert.ok(profile.endTime > profile.startTime);
    const byId = new Map(profile.nodes.map((n) => [n.id, n]));
    assert.equal(profile.nodes[0].callFrame.functionName, '(root)');
    for (const id of profile.samples) assert.ok(byId.has(id));
    // spin() under App, located where it is defined in the bundle.
    const spin = profile.nodes.find((n) => n.callFrame.functionName === 'spin');
    assert.ok(spin && spin.hitCount > 0, 'spin was sampled: ' + profile.nodes.map((n) => n.callFrame.functionName).join(','));
    assert.equal(spin.callFrame.url, parsed.params.url);
    assert.equal(spin.callFrame.scriptId, parsed.params.scriptId);
    const parentOf = (n) => profile.nodes.find((p) => p.children.includes(n.id));
    const parent = parentOf(spin);
    assert.equal(parent.callFrame.functionName, 'App');
    // Above the JS: what called into it, under the root.
    let top = parent;
    while (parentOf(top).callFrame.functionName !== '(root)') top = parentOf(top);
    assert.match(top.callFrame.functionName, /^\((timer|animation frame|microtasks|call: .*|event: .*)\)$/);
    const bundle = await (await fetch(parsed.params.url)).text();
    assert.match(bundle.split('\n')[spin.callFrame.lineNumber], /function spin\(/);
    assert.ok(profile.nodes.some((n) => n.callFrame.functionName === '(idle)' && n.hitCount > 0));
    // The runtime's own work between JS calls, in JS's idle time.
    const phases = profile.nodes.filter((n) => /^\((layout|paint|commit|style|gpu)\)$/.test(n.callFrame.functionName) && n.hitCount > 0);
    assert.ok(phases.length > 0, 'phases in the profile: ' + profile.nodes.map((n) => n.callFrame.functionName).join(','));
    for (const n of phases) assert.ok(profile.nodes[0].children.includes(n.id), n.callFrame.functionName + ' is top level');
    c.ws.close();
  } finally {
    child.kill();
    await s.close();
  }
});

test('network: the runtime reports its loads as Network events (native runtime)', { skip: !process.env.JSUI_RENDER && !existsSync(binary) && `no ${binary}`, timeout: 60000 }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-network-'));
  await writeFile(path.join(dir, 'App.jsx'), APP);
  await writeFile(path.join(dir, 'index.jsx'), "import { AppRegistry } from 'react-native';\nimport App from './App';\nAppRegistry.registerComponent('main', () => App);\n");
  const s = await dev({ entry: path.join(dir, 'index.jsx'), port: 0, log: () => {} });
  const origin = `http://127.0.0.1:${s.port}`;
  const child = spawn(binary, ['--dev', `${origin}/bundle.js`, '--wait-ms', '50000', '-o', path.join(dir, 'out.png')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', (d) => (output += d));
  child.stderr.on('data', (d) => (output += d));
  try {
    let target = null;
    for (let i = 0; i < 300 && !target; i++) {
      target = (await (await fetch(`${origin}/json/list`)).json())[0];
      if (!target) await sleep(50);
    }
    assert.ok(target, 'the runtime is listed\n' + output);
    const c = await connect(target.webSocketDebuggerUrl);
    await c.send('Runtime.enable');
    assert.deepEqual((await c.send('Network.enable')).result, {});
    assert.deepEqual((await c.send('Network.emulateNetworkConditionsByRule', { offline: false, matchedNetworkConditions: [] })).result, { ruleIds: [] });
    const seen = c.events.length;
    await c.send('Runtime.evaluate', { expression: `fetch(${JSON.stringify(origin + '/bundle.js')}, { headers: { 'X-Test': '1' } }).then((r) => r.text())` });
    const sent = await c.event((m) => m.method === 'Network.requestWillBeSent' && m.params.request.url.endsWith('/bundle.js'), seen);
    assert.equal(sent.params.type, 'Fetch');
    assert.equal(sent.params.request.method, 'GET');
    assert.equal(sent.params.request.headers['x-test'] ?? sent.params.request.headers['X-Test'], '1');
    const id = sent.params.requestId;
    const response = await c.event((m) => m.method === 'Network.responseReceived' && m.params.requestId === id, seen);
    assert.equal(response.params.response.status, 200);
    assert.equal(response.params.response.mimeType, 'text/javascript');
    const done = await c.event((m) => m.method === 'Network.loadingFinished' && m.params.requestId === id, seen);
    assert.ok(done.params.encodedDataLength > 1000);
    assert.ok(done.params.timestamp >= sent.params.timestamp);
    const body = (await c.send('Network.getResponseBody', { requestId: id })).result;
    assert.equal(body.base64Encoded, false);
    assert.match(body.body, /registerComponent/);
    // The dev server's own traffic (session polls, the debugger's) isn't the app's.
    assert.ok(!c.events.some((m) => m.method === 'Network.requestWillBeSent' && m.params.request.url.includes('/__jsui/')));
    c.ws.close();
  } finally {
    child.kill();
    await s.close();
  }
});
