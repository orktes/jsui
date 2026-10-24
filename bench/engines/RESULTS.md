# JS engine comparison for JSUI (Apple silicon, macOS)

Engine-only benchmark: `bench/engines/out/react-commit.js` (React 19.3 production + react-reconciler,
pure-JS host config, ES2017 IIFE, 512 KB unminified / 147 KB minified). Same scenarios as
`packages/react/bench/commit-bench.jsx`, without the native bridge. Harness: `bench/engines/run.py`
(raw numbers in `bench/engines/results.json`). Run on 2026-10-24.

Machine: Mac Studio, Apple M1 Ultra, macOS 26.5.2 (25F84), Apple clang 21.0.0. Runs were sequential with
nothing else building.

## Versions and flags

| Engine | Version | Build |
|---|---|---|
| QuickJS-ng (JSUI) | v0.17.0 (6d46d07), patched exactly like `cmake/Deps.cmake` on main after #84 (inline JS_FreeValue/DupValue, inline `===`) | `cc -O3 -DNDEBUG -std=gnu11 -arch arm64 -fPIC -w -D_GNU_SOURCE` (JSUI's `jsui_quickjs` flags, `JSUI_QUICKJS_FLAGS` empty) + `qjs.c`/`quickjs-libc.c` with the same flags |
| QuickJS-ng (stock) | v0.17.0, unpatched `quickjs.c` | same flags |
| Hermes | `hermes-v260318099.0.4` (d412d3bd, 2026-09-24): newest release tag, the Hermes V1 / static_h line RN uses now. GitHub "Releases" still lists v0.13.0 (2024) as latest | `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release` (JIT build adds `-DHERMESVM_ALLOW_JIT=1`) |
| Static Hermes | `shermes` from the same tag/tree | same build |
| JavaScriptCore | system framework 21624.2.5.11.8 (macOS 26.5.2), `Helpers/jsc` | Apple's |
| V8 (reference) | Node v26.0.0 (Homebrew) | |

## Results

¹ Hermes has no `performance.now`, so the bench falls back to `Date.now()` (1 ms resolution). The per-commit
numbers are averages over 20 to 100 commits, good to about 0.01 ms; its init line reads 0. Use the startup table.

### Speed (ms per commit, median of 5 runs; lower is better)

| Engine / mode | init | mount | plain none | plain tenth | plain all | memo none | memo tenth | memo all | vs JSUI QuickJS (plain none) |
|---|---|---|---|---|---|---|---|---|---|
| QuickJS-ng (JSUI patched) | 0.162 | 3.72 | 2.65 | 2.67 | 2.65 | 1.39 | 1.71 | 2.98 | 1.0x |
| QuickJS-ng bytecode exe (qjs -c) | 0.161 | 3.63 | 2.63 | 2.66 | 2.66 | 1.39 | 1.73 | 2.99 | 1.0x |
| QuickJS-ng (stock v0.17.0) | 0.167 | 3.75 | 2.69 | 2.72 | 2.72 | 1.43 | 1.77 | 3.07 | 1.0x |
| Hermes source | n/a¹ | 1.45 | 1.20 | 1.22 | 1.20 | 0.68 | 0.84 | 1.39 | 2.2x |
| Hermes .hbc | n/a¹ | 1.50 | 1.20 | 1.22 | 1.21 | 0.68 | 0.84 | 1.37 | 2.2x |
| Hermes .hbc + JIT (-Xjit=on) | n/a¹ | 0.90 | 0.60 | 0.60 | 0.60 | 0.39 | 0.46 | 0.74 | 4.4x |
| Static Hermes (shermes, untyped native) | n/a¹ | 0.95 | 0.70 | 0.71 | 0.71 | 0.40 | 0.49 | 0.86 | 3.8x |
| JSC full JIT (default) | 1.100 | 0.32 | 0.19 | 0.21 | 0.11 | 0.09 | 0.07 | 0.11 | 13.9x |
| JSC baseline JIT only | 1.080 | 0.64 | 0.48 | 0.49 | 0.46 | 0.23 | 0.29 | 0.52 | 5.6x |
| JSC LLInt only (--useJIT=false, = iOS app) | 1.040 | 2.26 | 1.98 | 2.03 | 2.00 | 0.96 | 1.20 | 2.16 | 1.3x |
| Node/V8 (reference) | 1.392 | 0.32 | 0.17 | 0.14 | 0.13 | 0.07 | 0.09 | 0.14 | 15.2x |
| Node/V8 --jitless | 1.344 | 2.01 | 1.73 | 1.73 | 1.73 | 0.78 | 0.99 | 1.80 | 1.5x |

### Startup (wall ms, process start to exit, median of 15)

| Engine / mode | trivial `print(1)` | load+init bundle (512 KB unminified) | load+init bundle (147 KB minified) | bundle cost over trivial |
|---|---|---|---|---|
| QuickJS-ng (JSUI patched) | 4.6 | 39.8 | 32.2 | 35.3 |
| QuickJS-ng bytecode exe (qjs -c) | 5.0 | 6.7 | 6.7 | 1.8 |
| QuickJS-ng (stock v0.17.0) | 5.0 | 40.3 | 32.7 | 35.3 |
| Hermes source | 9.7 | 264.7 | 262.6 | 255.0 |
| Hermes .hbc | 8.5 | 8.6 | 8.7 | 0.1 |
| Hermes .hbc + JIT (-Xjit=on) | 8.2 | 9.1 | 8.8 | 1.0 |
| Static Hermes (shermes, untyped native) | 9.6 | 9.9 | - | 0.3 |
| JSC full JIT (default) | 10.1 | 16.2 | 14.7 | 6.1 |
| JSC baseline JIT only | 10.5 | 15.8 | 14.5 | 5.4 |
| JSC LLInt only (--useJIT=false, = iOS app) | 9.5 | 15.1 | 13.7 | 5.6 |
| Node/V8 (reference) | 74.4 | 80.5 | 78.5 | 6.1 |
| Node/V8 --jitless | 72.4 | 80.3 | 77.4 | 7.9 |

### Memory (max RSS, MiB, median of 3)

| Engine / mode | trivial script (engine baseline) | bundle loaded + React init | full bench (500 views mounted, 2,600 commits) | full minus baseline |
|---|---|---|---|---|
| QuickJS-ng (JSUI patched) | 2.5 | 8.1 | 13.3 | 10.8 |
| QuickJS-ng bytecode exe (qjs -c) | 2.4 | 3.5 | 6.8 | 4.4 |
| QuickJS-ng (stock v0.17.0) | 2.5 | 8.0 | 12.2 | 9.7 |
| Hermes source | 9.7 | 41.7 | 68.2 | 58.5 |
| Hermes .hbc | 8.5 | 8.9 | 35.0 | 26.5 |
| Hermes .hbc + JIT (-Xjit=on) | 8.6 | 9.0 | 37.5 | 28.8 |
| Static Hermes (shermes, untyped native) | 9.7 | 10.2 | 37.0 | 27.4 |
| JSC full JIT (default) | 7.2 | 10.1 | 147.0 | 139.8 |
| JSC baseline JIT only | 7.3 | 10.2 | 53.9 | 46.6 |
| JSC LLInt only (--useJIT=false, = iOS app) | 6.5 | 9.1 | 42.2 | 35.7 |
| Node/V8 (reference) | 49.0 | 52.5 | 77.6 | 28.6 |
| Node/V8 --jitless | 46.6 | 49.9 | 66.3 | 19.7 |

"Bundle cost over trivial" = load + parse/compile + run React/reconciler module init + create root.

### Binary size (what an app ships; `strip -x`, bytes)

| Library | file (stripped) | `__TEXT` | gzip -9 | Notes |
|---|---|---|---|---|
| QuickJS-ng, JSUI flags: `libqjs.dylib` (quickjs+libregexp+libunicode+dtoa) | 1,030,392 | 950,272 | 486,077 | parser/compiler included; no deps beyond libSystem |
| Hermes `libhermesvm.dylib` (VM + compiler + JSI) | 3,198,496 | 3,047,424 | 1,378,116 | runs source or .hbc; links libc++ + CoreFoundation |
| Hermes `libhermesvmlean.dylib` (no compiler; .hbc only) | 1,872,216 | 1,736,704 | 804,358 | what you ship with precompiled bytecode |
| Hermes JIT build: `libhermesvm` / `libhermesvmlean` | 3,480,432 / 2,137,712 | 3,309,568 / 1,998,848 | 1,495,110 / 924,547 | JIT adds about 270 KB |
| Static Hermes: bundle compiled to native | 1,285,896 (the bundle's code, executable) | | | plus `libhermesvm` + `libshermes_console` at run time |
| JavaScriptCore | 0 added on macOS/iOS | 27.0 MiB `__TEXT` + ~5 MiB data in the dyld shared cache | | system framework on Apple only. On Windows/Linux/Android it must be bundled (not measured here, but it is at least this big) |

Bundle payload sizes (bytes, gzip -9 in brackets):

| Form | size |
|---|---|
| JS source, unminified | 512,518 (78,924) |
| JS source, minified (esbuild --minify) | 147,212 (45,355) |
| QuickJS bytecode (`qjs -c`, appended to the exe) | ~190,300 (105,256) |
| Hermes `.hbc` (`hermesc -O`) from unminified / minified | 252,486 (136,009) / 246,501 (131,197) |
| Static Hermes native code | 1,285,896 |

Compile times (median of 5): `hermesc -O` 272 ms (unminified) / 268 ms (minified); `hermesc -O0` 59 ms
(304,822-byte .hbc); `qjs -c` 24 ms; `shermes -O` 7.6 s (one run; it runs the C compiler).

### Real JSUI bridge for context (QuickJS-ng, same build)

`npx jsui build packages/react/bench/commit-bench.jsx` + `build-bench/jsui-render bundle.js --wait-ms 0`
(Release, CPU raster, median of 5). The engine-only QuickJS column is beside it, so the gap is the bridge,
native prop diffing, Yoga and the rest of the native work:

| Scenario | jsui-render (ms/commit) | engine-only qjs (ms/commit) | native + bridge share |
|---|---|---|---|
| mount 500 views | 6.00 | 3.72 | 2.28 |
| update plain (none) | 3.15 | 2.65 | 0.50 |
| update plain (tenth) | 3.18 | 2.67 | 0.51 |
| update plain (all) | 3.38 | 2.65 | 0.73 |
| update memo (none) | 1.40 | 1.39 | 0.01 |
| update memo (tenth) | 1.80 | 1.71 | 0.09 |
| update memo (all) | 3.73 | 2.98 | 0.75 |
| Animated setValue x500 | 1.45 | - | |

jsui-render max RSS for the whole run (Skia + QuickJS + React): 32.5 MiB.

## Notes

- **The engine dominates the update cost.** In JSUI, about 85% of a plain update commit is React running in QuickJS.
  Replacing the engine scales almost directly onto the commit time.
- **No-JIT engines (what iOS allows a third-party app):** Hermes's interpreter is 2.2x faster than JSUI's
  QuickJS. JSC's LLInt is 1.3x faster, and V8 --jitless 1.5x. With a JIT (desktop/Android): Hermes JIT 4.4x,
  JSC 5.6x (baseline only) to 14x (full). V8 is in the same range.
- **#84's QuickJS patches** gain only about 1.5% over stock v0.17.0 in this engine-only bench. PR #84 measured
  more inside JSUI, where the refcount-heavy bridge paths are; the patches are not the engine-choice lever.
- **Hermes needs `-Xes6-block-scoping`** (passed to `hermes`, `hermesc` and `shermes` here). Without it,
  Hermes V1 still gives `let`/`const` function scope: `for (let k of …) fs.push(() => k)` yields `c,c,c`.
  esbuild's own `__copyProps` helper depends on per-iteration bindings, so the bundle crashes with
  "true is not a function". React Native avoids this by lowering block scoping with Babel. A JSUI Hermes
  backend would need either the flag (it must also be on in the runtime config for eval) or a Babel pass.
- **Hermes from source is slow to start** (265 ms) because the `hermes` CLI compiles with full `-O` before
  running. With `-lazy`, startup is 21 ms, but the code then runs about 2x slower (plain none 2.57 ms).
  Hermes is meant to ship `.hbc`, and with `.hbc` the bundle costs ~0.1 ms on top of process start.
- **Static Hermes** built from the same tree and compiled the untyped bundle on the first try (7.6 s).
  Untyped native code is 3.8x QuickJS, between the Hermes interpreter and the Hermes JIT. Its main benefit is
  typed code, and the bundle's native code is 5x bigger than its .hbc.
- **Startup:** QuickJS parses the 512 KB source in ~35 ms (28 ms minified). Precompiled QuickJS bytecode
  (`qjs -c`; JSUI could do the same with `JS_WriteObject`/`JS_ReadObject`) cuts that to ~2 ms and halves
  RSS. JSC parses lazily, so it costs ~6 ms with source alone.
- **Memory:** QuickJS (refcounting + small heap) is by far the leanest: 13 MiB for the full run, 7 MiB with
  bytecode. Hermes's GC heap grows to ~35 MiB; Hades defaults are not tuned for small heaps
  (`-gc-max-heap` / `-gc-init-heap` are untested here). JSC with full JIT reaches 147 MiB. That is mostly
  its desktop GC heap-growth policy plus JIT code. LLInt-only is 42 MiB.
- **JSC caveats:** 0 bytes on Apple platforms, but the iOS case is LLInt only (1.3x QuickJS). On every
  non-Apple target (Windows, Linux, Android, consoles, WASM) it would have to be bundled, and it is the
  biggest engine here. Its C API (JSObjectRef) is stable and well documented.
- **The bench bundle changed:** React's scheduler picks setImmediate / MessageChannel / setTimeout at load
  time, and plain qjs has none of them, so the old bundle crashed under qjs. `build.sh` now wraps it so every
  engine sees the same never-firing `setTimeout` stub and no setImmediate/MessageChannel. The bench flushes
  synchronously, and host-update counts match across all engines (501 / 1 / 51 / 501).

## Reproduce

Artifacts were prepared under ~/Development/engine-bench (bin/ holds hermes, hermes-jit, hermesc,
shermes, qjs, qjs-stock). In run/: rc.js = out/react-commit.js, rc-init.js = same with `return;` after the
init log line, *.min.js = esbuild --minify, *.hbc = `hermesc -O -Xes6-block-scoping -emit-binary`,
*.qjsexe = `qjs -c`, *.sh-exe = `shermes -O -Xes6-block-scoping -o`, trivial.js = `print(1)`. Then
`EB=~/Development/engine-bench python3 bench/engines/run.py > results.json`.
