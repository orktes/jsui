#!/usr/bin/env node
// jsui CLI: build and dev-serve JSUI bundles.
import { parseArgs } from 'node:util';
import { build } from '../src/build.js';
import { dev } from '../src/dev.js';

const USAGE = `usage:
  jsui build <entry> [-o dist/bundle.js] [--minify] [--sourcemap] [--dev]
  jsui dev <entry> [--port 5173] [--host 127.0.0.1]

'react-native' imports are aliased to @orktes/jsui-react; JSX works in .js/.jsx/.tsx.`;

async function main(argv) {
  const [cmd, ...rest] = argv;
  if (!cmd || cmd === '-h' || cmd === '--help' || cmd === 'help') {
    console.log(USAGE);
    return 0;
  }
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
    },
  });
  if (values.help) {
    console.log(USAGE);
    return 0;
  }
  const entry = positionals[0];
  if (!entry) {
    console.error('jsui: missing <entry>\n' + USAGE);
    return 2;
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
