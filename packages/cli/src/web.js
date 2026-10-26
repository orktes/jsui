// `jsui build <entry> --web <dir>`: a static folder ready to deploy (any
// static host: Vercel, Netlify, GitHub Pages, S3, ...):
//
//   index.html                     a full-window page that runs the app (kept
//                                  if <dir> already has one, or --html <file>)
//   jsui-web.js, jsui-webgpu.js,   the browser host (@orktes/jsui-web)
//   jsui-textures.js, jsui-audio.js,
//   jsui-worker.js
//   dist/, dist-webgl/, dist-gpu/  every WASM build the package ships: CPU,
//                                  Skia on WebGL, Skia Graphite on WebGPU (+ both)
//   app/bundle.js, app/assets/*    the app
//
// The host picks the best module the browser can use, so WebGPU works with no
// setup. Every URL besides index.html carries a content hash (?v=<hash>), so a
// browser or CDN never mixes files of two deploys: jsui-web.js gets its own
// imports and the deployed modules' versions written in (DEPLOYED_MODULES).
import crypto from 'node:crypto';
import { existsSync } from 'node:fs';
import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);

export const WEB_MODULES = ['dist', 'dist-webgl', 'dist-gpu'];
const HOST_SCRIPTS = ['jsui-webgpu.js', 'jsui-textures.js', 'jsui-audio.js', 'jsui-worker.js'];

const hash = (...bufs) => {
  const h = crypto.createHash('sha256');
  for (const b of bufs) h.update(b);
  return h.digest('hex').slice(0, 16);
};

// Replaces every `from` in `text`; there must be at least one.
function replaceAll(text, from, to, file) {
  if (!text.includes(from)) throw new Error(`${file}: no ${JSON.stringify(from)} to rewrite (an @orktes/jsui-web of another version?)`);
  return text.split(from).join(to);
}

/** The installed @orktes/jsui-web directory. */
export function webPackageDir() {
  try {
    return path.dirname(require.resolve('@orktes/jsui-web/package.json'));
  } catch {
    throw new Error('@orktes/jsui-web is not installed (npm install @orktes/jsui-web)');
  }
}

/**
 * Writes the browser host and its WASM modules into `outDir`, plus an
 * index.html that runs `bundle` (a path relative to outDir) unless
 * `html` names a page of your own or outDir already has an index.html.
 * @returns {Promise<{modules: {[dir: string]: string}, index: 'written'|'kept'|'copied'}>}
 */
export async function writeWebHost({ outDir, bundle, webDir = webPackageDir(), html, title = 'JSUI' }) {
  await mkdir(outDir, { recursive: true });
  const modules = {};
  for (const dir of WEB_MODULES) {
    const from = path.join(webDir, dir);
    if (!existsSync(path.join(from, 'jsui.js')) || !existsSync(path.join(from, 'jsui.wasm'))) continue;
    const js = await readFile(path.join(from, 'jsui.js'));
    const wasm = await readFile(path.join(from, 'jsui.wasm'));
    await mkdir(path.join(outDir, dir), { recursive: true });
    await writeFile(path.join(outDir, dir, 'jsui.js'), js);
    await writeFile(path.join(outDir, dir, 'jsui.wasm'), wasm);
    modules[dir] = hash(js, wasm);
  }
  if (!Object.keys(modules).length) throw new Error(`${webDir} has no WASM modules (dist*/jsui.{js,wasm})`);

  let web = await readFile(path.join(webDir, 'jsui-web.js'), 'utf8');
  for (const name of HOST_SCRIPTS) {
    const src = await readFile(path.join(webDir, name));
    await writeFile(path.join(outDir, name), src);
    const v = `${name}?v=${hash(src)}`;
    web = name === 'jsui-worker.js'
      ? replaceAll(web, `import('./${name}')`, `import('./${v}')`, 'jsui-web.js')
      : replaceAll(web, `from './${name}'`, `from './${v}'`, 'jsui-web.js');
  }
  web = replaceAll(web, 'const DEPLOYED_MODULES = null;', `const DEPLOYED_MODULES = ${JSON.stringify(modules)};`, 'jsui-web.js');
  await writeFile(path.join(outDir, 'jsui-web.js'), web);

  const index = path.join(outDir, 'index.html');
  const bundleUrl = `./${bundle.split(path.sep).join('/')}?v=${hash(await readFile(path.join(outDir, bundle)))}`;
  const webUrl = `./jsui-web.js?v=${hash(web)}`;
  let state = 'written';
  let page;
  if (html) {
    page = await readFile(html, 'utf8');
    state = 'copied';
  } else if (existsSync(index)) {
    page = await readFile(index, 'utf8');
    state = 'kept';
  }
  if (page != null) {
    // A page of your own (or the one an earlier build wrote): point its
    // jsui-web.js import and bundle URL at this deploy's versions.
    page = page.replace(/(['"])\.\/jsui-web\.js(\?v=[0-9a-f]+)?\1/g, (_, q) => `${q}${webUrl}${q}`);
    page = page.replace(/(['"])\.\/app\/bundle\.js(\?v=[0-9a-f]+)?\1/g, (_, q) => `${q}${bundleUrl}${q}`);
  } else {
    page = defaultPage({ title, webUrl, bundleUrl });
  }
  await writeFile(index, page);
  return { modules, index: state };
}

const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);

function defaultPage({ title, webUrl, bundleUrl }) {
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>${esc(title)}</title>
  <style>
    html, body { margin: 0; height: 100%; overflow: hidden; background: #000; overscroll-behavior: none; }
    #ui { position: fixed; inset: 0; width: 100vw; height: 100vh; height: 100dvh; display: block; }
    #err { position: fixed; left: 12px; right: 12px; bottom: 12px; max-height: 40vh; overflow: auto; padding: 10px 12px;
           background: #3d1214ee; color: #ffdcd7; font: 12px/1.4 ui-monospace, monospace; white-space: pre-wrap; display: none; }
  </style>
</head>
<body>
  <canvas id="ui"></canvas>
  <div id="err"></div>
  <script type="module">
    // Generated by \`jsui build --web\`. ?renderer=webgpu|webgl|cpu forces a
    // backend; window.jsui.renderer says which one runs.
    import { createJsui } from '${webUrl}';
    const err = document.getElementById('err');
    const showError = (msg) => { err.textContent = msg; err.style.display = msg ? 'block' : 'none'; };
    try {
      const jsui = await createJsui({
        canvas: document.getElementById('ui'),
        bundleUrl: '${bundleUrl}',
        renderer: new URLSearchParams(location.search).get('renderer') || 'auto',
        // GPU pipelines to build while loading (\`jsui pipelines <dir>\` records them).
        pipelines: './pipelines-{format}.bin',
      });
      window.jsui = jsui;
      jsui.on('error', showError);
    } catch (e) {
      showError('Failed to start JSUI: ' + (e && e.stack || e));
    }
  </script>
</body>
</html>
`;
}
