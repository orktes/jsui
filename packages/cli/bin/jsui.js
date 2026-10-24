#!/usr/bin/env node
// jsui CLI: build and dev-serve JSUI bundles, render them headlessly
// (screenshot / inspect / run) and hand out the agent skill.
import { parseArgs } from 'node:util';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { build } from '../src/build.js';
import { dev } from '../src/dev.js';
import { elementSummary, formatTree, launch } from '../src/headless.js';
import { attach, sessionClients } from '../src/attach.js';
import { answerOf, parseInlineSteps, runSteps, STEP_HELP } from '../src/script.js';
import { skillCommand, SKILL_USAGE } from '../src/skill.js';

const USAGE = `usage:
  jsui build <entry> [-o dist/bundle.js] [--minify] [--sourcemap] [--dev]
     [--format combo|bytecode|source|split] [--react-compiler [--compiler-report]]
     What goes in the bundle (all load in every JSUI runtime of this version):
       combo     (default) QuickJS bytecode (faster startup, less memory) with
                 the source inside as the fallback for other JSUI versions;
                 ~2.2x the size of the JS
       bytecode  bytecode only: ~1.25x the JS, only this JSUI version loads it
       source    plain JS (browser JS engines, other tools)
       split     bytecode in <out>, the source next to it in
                 <name>.source.js; the runtime reads it only as the fallback
  jsui dev <entry> [--port 5173] [--host 127.0.0.1] [--no-hmr] [--no-inspect]
     [--react-compiler] [--headless [view options]]
     Every runtime that loads the bundle in dev mode joins the server's shared
     session: http://127.0.0.1:5173/shared in a browser, the native viewer
     (jsui-viewer --dev <url>/bundle.js), a Godot JsuiView in dev mode, ...
     Agents (--attach) drive the live one: the last to join, or --client.
     --headless also runs one inside the server (no window needed), streamed
     to /shared.
     Debug the app's JS (breakpoints in your JSX/TS, via source maps) from
     Chrome (chrome://inspect -> Configure... -> add 127.0.0.1:5173) or
     VS Code (launch.json: {"type": "node", "request": "attach", "port": 5173});
     the runtime must be built with the debugger. --no-inspect turns it off.

  jsui screenshot <entry> [-o screenshot.png] [--wait 1000] [view options]
  jsui inspect <entry> [--wait 1000] [--find <selector>] [--json] [view options]
  jsui run <entry> <script.json|script.mjs> [--keep-going] [view options]
  jsui run <entry> <steps as words: press Down click '#play' type Bob ...>
     view options: [--width 1280] [--height 720] [--scale 1] [--fps 60]
       [--component Name] [--props '{"json":1}'] [--state key='json' ...]
       [--bundle] (entry is a built bundle.js) [--dev] (development React)
       [--js quickjs|host] (the JS engine: QuickJS in WASM, the default, or
       Node's own V8; JSUI_JS=host sets the default)

  jsui screenshot|inspect|run --attach http://127.0.0.1:5173 ...  (no <entry>)
     [--client <name|id>]
     drive the live runtime of 'jsui dev' instead: the same commands, on the
     app the person sees (browser, native, game), in real time. Waits up to
     30 s for a runtime to join when none is connected.
  jsui clients --attach <url> [--json]
     the runtimes in the session (* = live)
  jsui log --attach <url> [--since <seq>] [--follow]
     what the person, agents and the UI did (clicks, keys, messages, calls)
     and which runtimes joined

${SKILL_USAGE}

'react-native' imports are aliased to @orktes/jsui-react; JSX works in .js/.jsx/.tsx.
jsui dev hot-updates edited components in place (React Fast Refresh, state is
kept); --no-hmr reloads the whole bundle on every change instead.
--react-compiler runs React Compiler on the app's sources (not node_modules):
components and hooks are memoized automatically (build, dev, screenshot /
inspect / run; --no-react-compiler is the default). Components it cannot
compile (they break the Rules of React) run as written: --compiler-report
lists them.
screenshot / inspect / run render with the CPU WebAssembly runtime in Node
(deterministic; no browser or GPU). 'jsui run --help' lists the script steps.`;

const VIEW_OPTIONS = {
  width: { type: 'string' },
  height: { type: 'string' },
  scale: { type: 'string' },
  fps: { type: 'string' },
  component: { type: 'string' },
  props: { type: 'string' },
  state: { type: 'string', multiple: true },
  bundle: { type: 'boolean' },
  js: { type: 'string' },
  wait: { type: 'string' },
  find: { type: 'string' },
  json: { type: 'boolean' },
  'keep-going': { type: 'boolean' },
  attach: { type: 'string' },
  client: { type: 'string' },
  headless: { type: 'boolean' },
  shared: { type: 'boolean' },  // the old name of --headless
  since: { type: 'string' },
  follow: { type: 'boolean' },
};

function num(v, name, fallback) {
  if (v == null) return fallback;
  const n = Number(v);
  if (!Number.isFinite(n) || n <= 0) throw new UsageError(`invalid --${name} ${v}`);
  return n;
}

class UsageError extends Error {}

// --react-compiler / --no-react-compiler; undefined keeps the default.
function reactCompilerOption(values) {
  if (values['no-react-compiler']) return false;
  if (values['react-compiler']) return true;
  return undefined;
}

function json(v, name) {
  try {
    return JSON.parse(v);
  } catch {
    throw new UsageError(`--${name} must be JSON: ${v}`);
  }
}

function jsEngine(v) {
  if (v == null) return undefined;
  if (v !== 'host' && v !== 'quickjs') throw new UsageError(`--js takes quickjs or host, got ${v}`);
  return v;
}

function viewOptions(values, entry) {
  const state = {};
  for (const kv of values.state || []) {
    const i = kv.indexOf('=');
    if (i <= 0) throw new UsageError(`--state takes key=json, got ${kv}`);
    const raw = kv.slice(i + 1);
    let v;
    try {
      v = JSON.parse(raw);
    } catch {
      v = raw;  // a bare string
    }
    state[kv.slice(0, i)] = v;
  }
  return {
    [values.bundle ? 'bundle' : 'entry']: entry,
    width: num(values.width, 'width', 1280),
    height: num(values.height, 'height', 720),
    scale: num(values.scale, 'scale', 1),
    fps: num(values.fps, 'fps', 60),
    component: values.component,
    props: values.props ? json(values.props, 'props') : undefined,
    state,
    dev: !!values.dev,
    reactCompiler: reactCompilerOption(values),
    js: jsEngine(values.js),
    onLog: (level, text) => {
      if (level >= 2) process.stderr.write((level >= 3 ? 'error: ' : 'warn: ') + text + '\n');
    },
  };
}

function reportErrors(app) {
  for (const e of app.errors) process.stderr.write('jsui: JS error: ' + e + '\n');
  return app.errors.length ? 1 : 0;
}

async function headless(cmd, values, positionals) {
  const attached = values.attach != null;
  // Attached, there is no <entry>: run's script is the first positional.
  if (attached) positionals = [null, ...positionals];
  const entry = positionals[0];
  // run: a script file, or the steps as words (`press Down click '#play'`).
  let inline = null;
  if (cmd === 'run') {
    const words = positionals.slice(1);
    if (!words.length) throw new UsageError('jsui run: missing <script.json|script.mjs> or steps (see jsui run --help)');
    if (!(words.length === 1 && /\.(json|mjs|js|cjs)$/.test(words[0]))) {
      try {
        inline = parseInlineSteps(words);
      } catch (e) {
        throw new UsageError('run: ' + e.message);
      }
    }
  }
  const app = attached ? await attach(values.attach, { client: values.client }) : await launch(viewOptions(values, entry));
  try {
    await app.ready();
    if (cmd === 'screenshot') {
      await app.wait(values.wait != null ? Number(values.wait) : attached ? 0 : 1000);
      const out = values.out || 'screenshot.png';
      await app.screenshot(out);
      console.log(`jsui: wrote ${out} (${app.width * app.scale}x${app.height * app.scale})`);
      return reportErrors(app);
    }
    if (cmd === 'inspect') {
      // `jsui inspect <entry> '#buy'`: a selector given without --find.
      if (values.find == null && positionals[1] != null) values.find = positionals[1];
      await app.wait(values.wait != null ? Number(values.wait) : attached ? 0 : 1000);
      if (values.find) {
        const found = (await app.findAll(values.find)).map(elementSummary);
        console.log(values.json ? JSON.stringify(found, null, 2) : found.map((el) => formatTree({ roots: [{ rootTag: '', children: [el] }] }).split('\n')[1].trim()).join('\n') || '(no match)');
      } else {
        const tree = await app.tree();
        console.log(values.json ? JSON.stringify(tree, null, 2) : formatTree(tree));
      }
      return reportErrors(app);
    }
    // run
    const scriptPath = positionals[1];
    if (!inline && /\.(mjs|js|cjs)$/.test(scriptPath)) {
      const mod = await import(pathToFileURL(path.resolve(scriptPath)).href);
      const fn = mod.default || mod.run;
      if (typeof fn !== 'function') throw new UsageError(`${scriptPath} must export default async function (app) {...}`);
      const result = await fn(app);
      console.log(JSON.stringify({ done: true, result: result ?? null, messages: app.messages, calls: app.calls, errors: app.errors }));
      // Attached, errors from before this script are the session's, not ours.
      return attached ? 0 : reportErrors(app);
    }
    const parsed = inline || JSON.parse(await readFile(scriptPath, 'utf8'));
    const steps = Array.isArray(parsed) ? parsed : parsed.steps;
    if (!Array.isArray(steps)) throw new UsageError(`${scriptPath}: expected an array of steps or {"steps": [...]}`);
    if (attached) {
      const r = await app.runSteps(steps, { keepGoing: !!values['keep-going'], calls: Array.isArray(parsed) ? undefined : parsed.calls });
      for (const out of r.results) console.log(JSON.stringify(out));
      console.log(JSON.stringify({ done: true, ok: r.ok, failed: r.failed, messages: app.messages, calls: app.calls, errors: app.errors }));
      return r.ok ? 0 : 1;
    }
    if (!Array.isArray(parsed) && parsed.calls) for (const [name, answer] of Object.entries(parsed.calls)) app.handle(name, answerOf(answer));
    const r = await runSteps(app, steps, {
      keepGoing: !!values['keep-going'],
      onResult: (out) => console.log(JSON.stringify(out)),
    });
    console.log(JSON.stringify({ done: true, ok: r.ok, failed: r.failed, messages: app.messages, calls: app.calls, errors: app.errors }));
    return r.ok ? reportErrors(app) : 1;
  } finally {
    app.close();
  }
}

// `jsui log --attach <url>`: the shared view's activity, one JSON line per event.
async function activityLog(values) {
  const app = await attach(values.attach);
  let since = values.since != null ? Number(values.since) || 0 : 0;
  for (;;) {
    const events = await app.activity(since);
    for (const e of events) {
      console.log(JSON.stringify(e));
      since = Math.max(since, e.seq);
    }
    if (!values.follow) return 0;
    await new Promise((r) => setTimeout(r, 500));
  }
}

async function main(argv) {
  const [cmd, ...rest] = argv;
  if (!cmd || cmd === '-h' || cmd === '--help' || cmd === 'help') {
    console.log(USAGE);
    return 0;
  }
  if (cmd === 'skill') return skillCommand(rest);
  const { values, positionals } = parseArgs({
    args: rest,
    allowPositionals: true,
    options: {
      out: { type: 'string', short: 'o' },
      minify: { type: 'boolean' },
      sourcemap: { type: 'boolean' },
      dev: { type: 'boolean' },
      port: { type: 'string', short: 'p' },
      host: { type: 'string' },
      'no-hmr': { type: 'boolean' },
      'no-inspect': { type: 'boolean' },
      format: { type: 'string' },
      'react-compiler': { type: 'boolean' },
      'no-react-compiler': { type: 'boolean' },
      'compiler-report': { type: 'boolean' },
      help: { type: 'boolean', short: 'h' },
      ...VIEW_OPTIONS,
    },
  });
  if (values.help) {
    console.log(cmd === 'run' ? STEP_HELP : USAGE);
    return 0;
  }
  if (cmd === 'clients') {
    if (values.attach == null) {
      console.error('jsui clients: --attach <url> is required');
      return 2;
    }
    try {
      const list = await sessionClients(values.attach);
      if (values.json) console.log(JSON.stringify(list, null, 2));
      else if (!list.length) console.log('(no runtime connected: open <url>/shared, the native viewer or the game in dev mode)');
      else for (const c of list) console.log(`${c.live ? '*' : ' '} ${c.id.slice(0, 8)}  ${c.label}  ${c.width}x${c.height}${c.scale !== 1 ? ' @' + c.scale + 'x' : ''}`);
      return 0;
    } catch (e) {
      console.error((e && e.message) || String(e));
      return 1;
    }
  }
  if (cmd === 'log') {
    if (values.attach == null) {
      console.error('jsui log: --attach <url> is required');
      return 2;
    }
    try {
      return await activityLog(values);
    } catch (e) {
      console.error((e && e.message) || String(e));
      return 1;
    }
  }
  if (values.attach != null && (cmd === 'screenshot' || cmd === 'inspect' || cmd === 'run')) {
    try {
      return await headless(cmd, values, positionals);
    } catch (e) {
      console.error(e instanceof UsageError ? 'jsui: ' + e.message : (e && e.message) || String(e));
      return e instanceof UsageError ? 2 : 1;
    }
  }
  const entry = positionals[0];
  if (!entry) {
    console.error('jsui: missing <entry>\n' + USAGE);
    return 2;
  }
  if (cmd === 'screenshot' || cmd === 'inspect' || cmd === 'run') {
    try {
      return await headless(cmd, values, positionals);
    } catch (e) {
      console.error(e instanceof UsageError ? 'jsui: ' + e.message : (e && e.message) || String(e));
      return e instanceof UsageError ? 2 : 1;
    }
  }
  if (cmd === 'build') {
    const t0 = Date.now();
    try {
      const r = await build({
        entry,
        outfile: values.out || 'dist/bundle.js',
        minify: !!values.minify,
        sourcemap: !!values.sourcemap,
        mode: values.dev ? 'development' : 'production',
        format: values.format || 'combo',
        reactCompiler: reactCompilerOption(values),
      });
      const rc = r.reactCompiler;
      const compiled = rc ? `, React Compiler: ${rc.functions} functions in ${rc.files} files${rc.skipped.length ? `, ${rc.skipped.length} skipped` : ''}` : '';
      const kind =
        r.format === 'combo' ? `bytecode + ${(r.sourceBytes / 1024).toFixed(1)} KiB source` : r.format === 'split' ? `bytecode, source in ${path.basename(r.sourceFile)}` : r.format;
      console.log(`jsui: wrote ${r.outfile} (${(r.bytes / 1024).toFixed(1)} KiB, ${kind}, ${r.assets.length} assets${compiled}) in ${Date.now() - t0} ms`);
      if (typeof r.bytecode === 'string') console.warn(`jsui: not precompiled (${r.bytecode}); pass --format source to silence this`);
      if (rc && values['compiler-report']) {
        for (const s of rc.skipped) console.log(`  skipped ${s.name || '(anonymous)'} (${s.where}${s.line ? `, line ${s.line}` : ''}): ${s.reason}`);
      }
      return 0;
    } catch (e) {
      console.error('jsui: build failed\n' + (e && e.message ? e.message : e));
      return 1;
    }
  }
  if (cmd === 'dev') {
    const port = values.port != null ? Number(values.port) : 5173;
    if (!Number.isInteger(port) || port < 0 || port > 65535) {
      console.error('jsui: invalid --port');
      return 2;
    }
    try {
      let headlessView = null;
      if (values.headless || values.shared) {
        const v = viewOptions(values, entry);
        headlessView = { width: v.width, height: v.height, scale: v.scale, fps: v.fps, component: v.component, props: v.props, state: v.state, js: v.js };
      }
      const server = await dev({ entry, port, host: values.host || '127.0.0.1', hmr: !values['no-hmr'], inspect: !values['no-inspect'], reactCompiler: reactCompilerOption(values), headless: headlessView });
      const stop = async () => {
        await server.close();
        process.exit(0);
      };
      process.on('SIGINT', stop);
      process.on('SIGTERM', stop);
      return null;  // keep running
    } catch (e) {
      if (e && e.code === 'EADDRINUSE') console.error(`jsui: port ${port} is in use (try --port)`);
      else console.error('jsui: dev server failed\n' + (e && e.message ? e.message : e));
      return 1;
    }
  }
  console.error(`jsui: unknown command '${cmd}'\n` + USAGE);
  return 2;
}

main(process.argv.slice(2)).then((code) => {
  if (code !== null) process.exitCode = code;
});
