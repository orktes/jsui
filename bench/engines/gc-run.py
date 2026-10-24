#!/usr/bin/env python3
"""Runs out/gc-bench.js (as $EB/run/gc.js / gc.hbc) on each engine; prints JSON (medians of RUNS).
  EB=~/Development/engine-bench python3 bench/engines/gc-run.py [filters] > gc-results.json
"""
import json, os, re, statistics, subprocess, sys

EB = os.path.expanduser(os.environ.get('EB', '~/Development/engine-bench'))
BIN, RUN = f'{EB}/bin', f'{EB}/run'
RUNS = 3
HS = ['-gc-print-stats']
MODES = {
  'QuickJS-ng JSUI v0.17.0 (current)':       ({}, [f'{BIN}/qjs', 'gc.js']),
  'QuickJS-ng rnq17 + JSUI, default GC':      ({}, [f'{BIN}/qjs-rnq17-jsui', 'gc.js']),
  'QuickJS-ng rnq17 + JSUI, deferred GC':     ({'QJS_GC_DEFERRED': '1'}, [f'{BIN}/qjs-rnq17-jsui-gc', 'gc.js']),
  'Hermes .hbc (interpreter)':                ({}, [f'{BIN}/hermes', *HS, 'gc.hbc']),
  'Hermes .hbc + JIT (-Xjit=on)':             ({}, [f'{BIN}/hermes-jit', '-Xjit=on', *HS, 'gc.hbc']),
}

def once(env, cmd):
  r = subprocess.run(['/usr/bin/time', '-l', *cmd], cwd=RUN, env={**os.environ, **env}, capture_output=True, text=True, check=True)
  out, err = r.stdout, r.stderr
  d = {'rss': int(re.search(r'(\d+)\s+maximum resident set size', err).group(1)) / 2**20}
  for key, pat in (('frame', r'\[gc\] frame ms: (.*)'), ('between', r'\[gc\] between-frame GC ms: (.*)')):
    m = re.search(pat, out)
    if m:
      for k, v in re.findall(r'(\w+)=([\d.]+)', m.group(1)): d[f'{key}_{k}'] = float(v)
  m = re.search(r'\[gc\] total ([\d.]+) ms', out); d['total'] = float(m.group(1))
  for k in ('totalGCTime', 'maxGCPause', 'numCollections', 'avgGCPause'):
    m = re.search(rf'"{k}": ([\d.e-]+)', out + err)
    if m: d['hermes_' + k] = float(m.group(1))
  return d

only = sys.argv[1:]
res = {}
for name, (env, cmd) in MODES.items():
  if only and not any(o.lower() in name.lower() for o in only): continue
  print(name, file=sys.stderr)
  runs = [once(env, cmd) for _ in range(RUNS)]
  res[name] = {k: statistics.median(r[k] for r in runs) for k in runs[0]}
  res[name]['runs'] = runs
print(json.dumps(res, indent=1))
