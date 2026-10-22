// Self-contained HTML report (images inlined as data URIs).
import fs from 'node:fs';

const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);

function dataUri(file) {
  if (!file || !fs.existsSync(file)) return null;
  return 'data:image/png;base64,' + fs.readFileSync(file).toString('base64');
}

function panel(label, file, role) {
  const uri = dataUri(file);
  return `<figure class="panel ${role}"><figcaption>${label}</figcaption>${
    uri ? `<img loading="lazy" data-role="${role}" src="${uri}" alt="${esc(label)}">` : '<div class="missing">not rendered</div>'
  }</figure>`;
}

function caseCard(r) {
  const pct = r.mismatchPct == null ? '—' : `${r.mismatchPct.toFixed(2)}%`;
  const swipe =
    r.expectedPng && r.actualPng && fs.existsSync(r.actualPng)
      ? `<div class="swipe" style="--w:${r.meta.width};--h:${r.meta.height}">
          <img data-from="expected" alt="">
          <div class="swipe-top"><img data-from="actual" alt=""></div>
          <input type="range" min="0" max="100" value="50" aria-label="Swipe between expected and actual">
        </div>`
      : '';
  const logs = [...(r.webLogs || []), ...(r.jsuiLog ? [r.jsuiLog] : [])].join('\n').trim();
  return `<section class="case ${r.status}" data-status="${r.status}">
  <header>
    <span class="status">${r.status}</span>
    <h2>${esc(r.name)}</h2>
    <span class="dim">${r.meta.width}×${r.meta.height} @${r.scale}x</span>
    <span class="grow"></span>
    <span class="pct ${r.status}">${pct}</span>
    <span class="dim">max ${r.meta.maxMismatch}%</span>
  </header>
  ${r.error ? `<pre class="error">${esc(r.error)}</pre>` : r.notes ? `<pre>${esc(r.notes.join('\n'))}</pre>` : ''}
  <div class="panels">
    ${panel('Expected · react-native-web / Chromium', r.expectedPng, 'expected')}
    ${r.chromeOnly ? '' : panel('Actual · jsui-render', r.actualPng, 'actual')}
    ${r.chromeOnly ? '' : panel('Diff', r.diffPng, 'diff')}
  </div>
  ${r.chromeOnly ? '' : swipe ? `<details><summary>Swipe compare (left: actual, right: expected)</summary>${swipe}</details>` : ''}
  ${logs ? `<details><summary>Logs</summary><pre>${esc(logs)}</pre></details>` : ''}
</section>`;
}

export function writeReport(file, { results, info }) {
  const counts = results.reduce((m, r) => ((m[r.status] = (m[r.status] || 0) + 1), m), {});
  const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>JSUI reference report</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#2a313c;--text:#e6edf3;--dim:#8b949e;--pass:#3fb950;--fail:#f85149;--error:#d29922;--rendered:#58a6ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
.top{position:sticky;top:0;z-index:2;background:rgba(13,17,23,.92);backdrop-filter:blur(6px);border-bottom:1px solid var(--line);padding:14px 20px;display:flex;flex-wrap:wrap;gap:10px 18px;align-items:center}
.top h1{font-size:17px;margin:0 8px 0 0}
.chip{border:1px solid var(--line);border-radius:999px;padding:3px 10px;color:var(--dim);background:none;font:inherit;cursor:pointer}
.chip[aria-pressed=true]{color:var(--text);border-color:var(--dim)}
.chip b{color:var(--text)}
main{padding:18px 20px 60px;display:grid;gap:18px}
.case{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
.case.fail{border-color:color-mix(in srgb,var(--fail) 45%,var(--line))}
.case.error{border-color:color-mix(in srgb,var(--error) 45%,var(--line))}
.case header{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;margin-bottom:10px}
.case h2{font-size:15px;margin:0;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.grow{flex:1}.dim{color:var(--dim);font-size:12px}
.status{font-size:11px;text-transform:uppercase;letter-spacing:.06em;padding:2px 8px;border-radius:4px;background:var(--line)}
.pass .status{background:color-mix(in srgb,var(--pass) 25%,transparent);color:var(--pass)}
.fail .status{background:color-mix(in srgb,var(--fail) 25%,transparent);color:var(--fail)}
.error .status{background:color-mix(in srgb,var(--error) 25%,transparent);color:var(--error)}
.rendered .status{background:color-mix(in srgb,var(--rendered) 25%,transparent);color:var(--rendered)}
.pct{font-weight:700;font-variant-numeric:tabular-nums}.pct.pass{color:var(--pass)}.pct.fail{color:var(--fail)}
.panels{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px}
.panel{margin:0}.panel figcaption{color:var(--dim);font-size:12px;margin-bottom:4px}
.panel img{display:block;width:100%;height:auto;border:1px solid var(--line);border-radius:4px;background:repeating-conic-gradient(#222 0 25%,#2b2b2b 0 50%) 0 0/16px 16px;image-rendering:auto}
.panel img:hover{image-rendering:pixelated}
.missing{border:1px dashed var(--line);border-radius:4px;padding:40px;text-align:center;color:var(--dim)}
pre{white-space:pre-wrap;word-break:break-word;font-size:12px;background:#0b0f14;border:1px solid var(--line);border-radius:6px;padding:10px;max-height:320px;overflow:auto}
pre.error{color:var(--error)}
details{margin-top:10px}summary{cursor:pointer;color:var(--dim)}
.swipe{position:relative;margin-top:8px;max-width:calc(var(--w)*1px);aspect-ratio:var(--w)/var(--h)}
.swipe>img,.swipe-top img{position:absolute;inset:0;width:100%;height:100%}
.swipe-top{position:absolute;inset:0;overflow:hidden;clip-path:inset(0 calc(100% - var(--pos,50%)) 0 0);border-right:1px solid #fff}
.swipe input{position:absolute;left:0;right:0;bottom:-28px;width:100%}
.swipe{margin-bottom:36px}
@media (prefers-color-scheme: light){:root{--bg:#f6f8fa;--card:#fff;--line:#d0d7de;--text:#1f2328;--dim:#59636e}pre{background:#f6f8fa}.top{background:rgba(246,248,250,.92)}}
</style></head><body>
<div class="top">
  <h1>JSUI reference report</h1>
  <button class="chip" data-filter="all" aria-pressed="true">all <b>${results.length}</b></button>
  ${['pass', 'fail', 'error', 'rendered'].filter((s) => counts[s]).map((s) => `<button class="chip" data-filter="${s}" aria-pressed="false">${s} <b>${counts[s]}</b></button>`).join('')}
  <span class="grow"></span>
  <span class="dim">${esc(info)}</span>
</div>
<main>
${results.map(caseCard).join('\n')}
</main>
<script>
document.querySelectorAll('.chip').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('.chip').forEach(c=>c.setAttribute('aria-pressed',c===b));
  const f=b.dataset.filter;
  document.querySelectorAll('.case').forEach(c=>c.hidden=f!=='all'&&c.dataset.status!==f);
});
// Swipe images reuse the panel data URIs instead of embedding them twice.
document.querySelectorAll('.swipe img[data-from]').forEach(img=>{
  const src=img.closest('.case').querySelector('img[data-role="'+img.dataset.from+'"]');
  if(src) img.src=src.src;
});
document.querySelectorAll('.swipe input').forEach(i=>i.oninput=()=>i.parentElement.style.setProperty('--pos',i.value+'%'));
</script>
</body></html>`;
  fs.writeFileSync(file, html);
}
