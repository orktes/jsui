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
import { answerOf, runSteps, STEP_HELP } from '../src/script.js';
import { skillCommand, SKILL_USAGE } from '../src/skill.js';

const USAGE = `usage:
  jsui build <entry> [-o dist/bundle.js] [--minify] [--sourcemap] [--dev]
  jsui dev <entry> [--port 5173] [--host 127.0.0.1]

  jsui screenshot <entry> [-o screenshot.png] [--wait 1000] [view options]
  jsui inspect <entry> [--wait 1000] [--find <selector>] [--json] [view options]
  jsui run <entry> <script.json|script.mjs> [--keep-going] [view options]
     view options: [--width 1280] [--height 720] [--scale 1] [--fps 60]
       [--component Name] [--props '{"json":1}'] [--state key='json' ...]
       [--bundle] (entry is a built bundle.js) [--dev] (development React)

${SKILL_USAGE}

'react-native' imports are aliased to @orktes/jsui-react; JSX works in .js/.jsx/.tsx.
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
  wait: { type: 'string' },
  find: { type: 'string' },
  json: { type: 'boolean' },
  'keep-going': { type: 'boolean' },
};

function num(v, name, fallback) {
  if (v == null) return fallback;
  const n = Number(v);
  if (!Number.isFinite(n) || n <= 0) throw new UsageError(`invalid --${name} ${v}`);
  return n;
}

class UsageError extends Error {}

function json(v, name) {
  try {
    return JSON.parse(v);
  } catch {
    throw new UsageError(`--${name} must be JSON: ${v}`);
  }
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
  const entry = positionals[0];
  const app = await launch(viewOptions(values, entry));
  try {
    await app.ready();
    if (cmd === 'screenshot') {
      await app.wait(values.wait != null ? Number(values.wait) : 1000);
      const out = values.out || 'screenshot.png';
      await app.screenshot(out);
      console.log(`jsui: wrote ${out} (${app.width * app.scale}x${app.height * app.scale})`);
      return reportErrors(app);
    }
    if (cmd === 'inspect') {
      await app.wait(values.wait != null ? Number(values.wait) : 1000);
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
    if (!scriptPath) throw new UsageError('jsui run: missing <script.json|script.mjs>');
    if (/\.(mjs|js|cjs)$/.test(scriptPath)) {
      const mod = await import(pathToFileURL(path.resolve(scriptPath)).href);
      const fn = mod.default || mod.run;
      if (typeof fn !== 'function') throw new UsageError(`${scriptPath} must export default async function (app) {...}`);
      const result = await fn(app);
      console.log(JSON.stringify({ done: true, result: result ?? null, messages: app.messages, calls: app.calls, errors: app.errors }));
      return reportErrors(app);
    }
    const parsed = JSON.parse(await readFile(scriptPath, 'utf8'));
    const steps = Array.isArray(parsed) ? parsed : parsed.steps;
    if (!Array.isArray(parsed) && parsed.calls) for (const [name, answer] of Object.entries(parsed.calls)) app.handle(name, answerOf(answer));
    if (!Array.isArray(steps)) throw new UsageError(`${scriptPath}: expected an array of steps or {"steps": [...]}`);
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
      help: { type: 'boolean', short: 'h' },
      ...VIEW_OPTIONS,
    },
  });
  if (values.help) {
    console.log(cmd === 'run' ? STEP_HELP : USAGE);
    return 0;
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
      });
      console.log(`jsui: wrote ${r.outfile} (${(r.bytes / 1024).toFixed(1)} KiB, ${r.assets.length} assets) in ${Date.now() - t0} ms`);
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
      const server = await dev({ entry, port, host: values.host || '127.0.0.1' });
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
