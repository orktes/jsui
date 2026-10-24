#!/usr/bin/env python3
"""Runs out/react-commit.js on every engine/mode and prints markdown tables.

Expects the artifacts prepared as described in RESULTS.md, in $EB/run (bundles,
.hbc files, qjs/shermes executables) and the engine binaries in $EB/bin.
  EB=~/Development/engine-bench python3 bench/engines/run.py > results.md
"""
import json, os, re, statistics, subprocess, sys, time

EB = os.path.expanduser(os.environ.get('EB', '~/Development/engine-bench'))
BIN, RUN = f'{EB}/bin', f'{EB}/run'
JSC = '/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc'
HX = ['-Xes6-block-scoping']  # Hermes gives `let` function scope without it (see RESULTS.md)
SPEED_RUNS, START_RUNS, MEM_RUNS = 5, 15, 3

# name -> (full bench, init-only (unminified), init-only (minified), trivial print(1))
MODES = {
  'QuickJS-ng (JSUI patched)':        ([f'{BIN}/qjs', 'rc.js'], [f'{BIN}/qjs', 'rc-init.js'], [f'{BIN}/qjs', 'rc-init.min.js'], [f'{BIN}/qjs', 'trivial.js']),
  'QuickJS-ng bytecode exe (qjs -c)': (['./rc.qjsexe'], ['./rc-init.qjsexe'], ['./rc-init.min.qjsexe'], ['./trivial.qjsexe']),
  'QuickJS-ng (stock v0.17.0)':       ([f'{BIN}/qjs-stock', 'rc.js'], [f'{BIN}/qjs-stock', 'rc-init.js'], [f'{BIN}/qjs-stock', 'rc-init.min.js'], [f'{BIN}/qjs-stock', 'trivial.js']),
  'QuickJS-ng df836d1 (rnq base, unpatched)': ([f'{BIN}/qjs-df836d1', 'rc.js'], [f'{BIN}/qjs-df836d1', 'rc-init.js'], [f'{BIN}/qjs-df836d1', 'rc-init.min.js'], [f'{BIN}/qjs-df836d1', 'trivial.js']),
  'QuickJS-ng rnq (react-native-quickjs c8102d4)': ([f'{BIN}/qjs-rnq', 'rc.js'], [f'{BIN}/qjs-rnq', 'rc-init.js'], [f'{BIN}/qjs-rnq', 'rc-init.min.js'], [f'{BIN}/qjs-rnq', 'trivial.js']),
  'QuickJS-ng rnq + JSUI patches':    ([f'{BIN}/qjs-rnq-jsui', 'rc.js'], [f'{BIN}/qjs-rnq-jsui', 'rc-init.js'], [f'{BIN}/qjs-rnq-jsui', 'rc-init.min.js'], [f'{BIN}/qjs-rnq-jsui', 'trivial.js']),
  'QuickJS-ng rnq17 + JSUI patches (v0.17.0 port)': ([f'{BIN}/qjs-rnq17-jsui', 'rc.js'], [f'{BIN}/qjs-rnq17-jsui', 'rc-init.js'], [f'{BIN}/qjs-rnq17-jsui', 'rc-init.min.js'], [f'{BIN}/qjs-rnq17-jsui', 'trivial.js']),
  'QuickJS-ng rnq17 + JSUI bytecode exe (qjs -c)': (['./rc.rnq17.qjsexe'], ['./rc-init.rnq17.qjsexe'], ['./rc-init.min.rnq17.qjsexe'], ['./trivial.rnq17.qjsexe']),
  'Hermes source':                    ([f'{BIN}/hermes', *HX, 'rc.js'], [f'{BIN}/hermes', *HX, 'rc-init.js'], [f'{BIN}/hermes', *HX, 'rc-init.min.js'], [f'{BIN}/hermes', 'trivial.js']),
  'Hermes .hbc':                      ([f'{BIN}/hermes', 'rc.hbc'], [f'{BIN}/hermes', 'rc-init.hbc'], [f'{BIN}/hermes', 'rc-init.min.hbc'], [f'{BIN}/hermes', 'trivial.hbc']),
  'Hermes .hbc + JIT (-Xjit=on)':     ([f'{BIN}/hermes-jit', '-Xjit=on', 'rc.hbc'], [f'{BIN}/hermes-jit', '-Xjit=on', 'rc-init.hbc'], [f'{BIN}/hermes-jit', '-Xjit=on', 'rc-init.min.hbc'], [f'{BIN}/hermes-jit', '-Xjit=on', 'trivial.hbc']),
  'Static Hermes (shermes, untyped native)': (['./rc.sh-exe'], ['./rc-init.sh-exe'], None, ['./trivial.sh-exe']),
  'JSC full JIT (default)':           ([JSC, 'rc.js'], [JSC, 'rc-init.js'], [JSC, 'rc-init.min.js'], [JSC, 'trivial.js']),
  'JSC baseline JIT only':            ([JSC, '--useDFGJIT=false', '--useFTLJIT=false', 'rc.js'], [JSC, '--useDFGJIT=false', '--useFTLJIT=false', 'rc-init.js'], [JSC, '--useDFGJIT=false', '--useFTLJIT=false', 'rc-init.min.js'], [JSC, '--useDFGJIT=false', '--useFTLJIT=false', 'trivial.js']),
  'JSC LLInt only (--useJIT=false, = iOS app)': ([JSC, '--useJIT=false', 'rc.js'], [JSC, '--useJIT=false', 'rc-init.js'], [JSC, '--useJIT=false', 'rc-init.min.js'], [JSC, '--useJIT=false', 'trivial.js']),
  'Node/V8 (reference)':              (['node', 'rc.js'], ['node', 'rc-init.js'], ['node', 'rc-init.min.js'], ['node', 'trivial-node.js']),
  'Node/V8 --jitless':                (['node', '--jitless', 'rc.js'], ['node', '--jitless', 'rc-init.js'], ['node', '--jitless', 'rc-init.min.js'], ['node', '--jitless', 'trivial-node.js']),
}
SCEN = ['init', 'mount 500 views', 'update plain (none)', 'update plain (tenth)', 'update plain (all)',
        'update memo (none)', 'update memo (tenth)', 'update memo (all)', 'total']

def run(cmd):
  return subprocess.run(cmd, cwd=RUN, capture_output=True, text=True, check=True).stdout

def wall(cmd):
  t = time.perf_counter(); run(cmd); return (time.perf_counter() - t) * 1000

def maxrss(cmd):
  r = subprocess.run(['/usr/bin/time', '-l', *cmd], cwd=RUN, capture_output=True, text=True, check=True)
  return int(re.search(r'(\d+)\s+maximum resident set size', r.stderr).group(1)) / 2**20

def parse(out):
  d = {}
  for line in out.splitlines():
    m = re.match(r'\[bench\] ([^:]+?)(?: \(React.*\))?: ([\d.]+) ms', line)
    if m: d[m.group(1)] = float(m.group(2))
  return d

res = {}
only = sys.argv[1:]
for name, (full, init, initmin, triv) in MODES.items():
  if only and not any(o.lower() in name.lower() for o in only): continue
  print(name, file=sys.stderr)
  runs = [parse(run(full)) for _ in range(SPEED_RUNS)]
  speed = {s: statistics.median(r[s] for r in runs) for s in SCEN if all(s in r for r in runs)}
  for c in (init, triv): run(c)  # warm the file cache
  r = {'speed': speed,
       'start_init': statistics.median(wall(init) for _ in range(START_RUNS)),
       'start_init_min': statistics.median(wall(initmin) for _ in range(START_RUNS)) if initmin else None,
       'start_trivial': statistics.median(wall(triv) for _ in range(START_RUNS)),
       'rss_full': statistics.median(maxrss(full) for _ in range(MEM_RUNS)),
       'rss_init': statistics.median(maxrss(init) for _ in range(MEM_RUNS)),
       'rss_trivial': statistics.median(maxrss(triv) for _ in range(MEM_RUNS))}
  res[name] = r
print(json.dumps(res, indent=1))
