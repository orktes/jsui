# react-native-quickjs engine patches vs JSUI's QuickJS (Apple silicon, macOS)

[react-native-quickjs](https://github.com/react-native-quickjs/quickjs) (MIT) carries 72 engine patches on
quickjs-ng df836d1 (v0.16.2+17): inline caches, object-literal and closure shape templates, opcode fusions,
string caches, JSON and RegExp fast paths, lazy bytecode function bodies, a CRC32C bytecode checksum, deferred
GC, a debugger interface and more (their `engine/patches/README.md` explains each one). This file measures them
with the same engine-only React bench as `RESULTS.md` (`out/react-commit.js`, harness `run.py`), plus a new
GC-pause bench (`gc-bench.jsx`, harness `gc-run.py`), and ends with a head-to-head against Hermes.

Run on 2026-10-24, Mac Studio M1 Ultra, macOS 26.5.2, Apple clang 21.0.0. Runs were sequential and nothing
was building. One unrelated `node -e for(;;){}` process kept one of the 20 cores busy the whole time, but
the numbers match `RESULTS.md` (taken without it) within 1%. Raw numbers: `results-rnquickjs.json` (speed,
startup and RSS; the third of three runs, which agree within 1%), `results-hermes-rerun.json`, `gc-results.json`.

## Builds

All are built the same way as `bin/qjs` in `RESULTS.md`: every `.c` file is compiled with
`cc -O3 -DNDEBUG -std=gnu11 -arch arm64 -fPIC -w -D_GNU_SOURCE`, with `qjs.c`, `quickjs-libc.c` and `gen/repl.c`/`gen/standalone.c`
from the matching quickjs-ng tree. No PGO, LTO or `JSUI_QUICKJS_FLAGS`.

| Binary | Engine sources | Extra defines |
|---|---|---|
| `qjs` | quickjs-ng v0.17.0 + JSUI's two `cmake/Deps.cmake` patches (inline `JS_FreeValue`/`JS_DupValue`, inline `===`). This is JSUI today, minus PGO | none |
| `qjs-stock` | quickjs-ng v0.17.0, unpatched | none |
| `qjs-df836d1` | quickjs-ng df836d1, unpatched (rnq's base) | none |
| `qjs-rnq` | rnq c8102d4 `engine/quickjs-rel/*` (the 72 patches applied), with `-I quickjs-rel` before `-I quickjs-ng` | `-DJS_ENABLE_DEBUGGER=1` |
| `qjs-rnq-jsui` | same as above + JSUI's two Deps.cmake patches | `-DJS_ENABLE_DEBUGGER=1` |
| `qjs-rnq17-jsui` | quickjs-ng v0.17.0 + `third_party/quickjs-rnq/*.patch` from `claude/wip-rnquickjs` (1373825), applied with `git am` (72 patches incl. 9999, BC_VERSION 29), + JSUI's two Deps.cmake patches. **This is the JSUI candidate** | `-DJS_ENABLE_DEBUGGER=1` |
| `qjs-rnq17-jsui-gc` | same engine objects as `qjs-rnq17-jsui`; only `qjs.c` differs: with `QJS_GC_DEFERRED=1` it calls `JS_SetGCDeferred` and adds a global `__runPendingGC()` (see the GC section) | `-DJS_ENABLE_DEBUGGER=1` |

Defines: rnq's release builds (`cmake/quickjs.cmake`, the podspec) define only `_GNU_SOURCE` and
`JS_ENABLE_DEBUGGER=1` (the debugger hooks cost nothing until a trace handler is installed). Lazy JSON
(`QJS_JSON_LAZY_DEFAULT`) is opt-in and stays off. The inline caches are on by default in the source
(`JS_ENABLE_IC 1`, 4 entries). The CRC32C bytecode checksum uses `__ARM_FEATURE_CRC32`, which `-arch arm64` already
defines on Apple clang, so no flag is needed.

JSUI's Deps.cmake patches were reproduced exactly: the `quickjs.c` block of `cmake/Deps.cmake` on main
was extracted into a `cmake -P` script with the same `string(REPLACE)` and `file(CONFIGURE @ONLY)` calls. **Both
anchors and all four `JS_FreeValue*`/`JS_DupValue*` definitions matched** on rnq's `quickjs-rel/quickjs.c`, on
v0.17.0, and on the v0.17.0 port. The bodies the patch replaces are unchanged by rnq.

Caveat: rnq bumps `BC_VERSION`, and `qjs-rnq`/`qjs-rnq-jsui` use quickjs-ng df836d1's `gen/repl.c`/`gen/standalone.c`
(old bytecode), so their REPL and `qjs -c` would fail. Running scripts is not affected. The v0.17.0 port
regenerates `gen/*.c`, so `qjs-rnq17-jsui -c` works and is benchmarked below.

## Speed (ms per commit, median of 5 runs; lower is better)

| Build | init | mount 500 | plain none | plain tenth | plain all | memo none | memo tenth | memo all |
|---|---|---|---|---|---|---|---|---|
| JSUI v0.17.0 patched (`qjs`) | 0.164 | 3.69 | 2.61 | 2.66 | 2.64 | 1.39 | 1.72 | 2.99 |
| JSUI v0.17.0 patched, bytecode exe | 0.158 | 3.64 | 2.64 | 2.65 | 2.65 | 1.39 | 1.72 | 2.97 |
| stock v0.17.0 | 0.170 | 3.72 | 2.69 | 2.72 | 2.72 | 1.43 | 1.77 | 3.06 |
| df836d1 (rnq base) | 0.161 | 3.80 | 2.72 | 2.76 | 2.75 | 1.45 | 1.79 | 3.10 |
| rnq c8102d4 | 0.141 | 2.93 | 2.40 | 2.42 | 2.42 | 1.24 | 1.53 | 2.68 |
| rnq + JSUI patches | 0.153 | 2.85 | 2.35 | 2.36 | 2.34 | 1.21 | 1.49 | 2.64 |
| **rnq17 + JSUI patches (v0.17.0 port)** | 0.152 | **2.83** | **2.33** | 2.35 | 2.34 | **1.21** | 1.50 | 2.65 |
| rnq17 + JSUI patches, bytecode exe | 0.148 | 2.83 | 2.35 | 2.34 | 2.33 | 1.22 | 1.52 | 2.65 |

Plain-none update over three full runs: JSUI `qjs` 2.644 / 2.639 / 2.613, rnq + JSUI 2.373 / 2.364 / 2.349.

- **The rnq patches against their own base (df836d1):** mount -23%, plain updates -12%, memo updates -14%.
- **The v0.17.0 port with JSUI's patches against JSUI today (`qjs`):** mount -23% (3.69 → 2.83), plain none -11%
  (2.61 → 2.33), memo none -13%, memo all -11%. The df836d1 and v0.17.0 ports run the same.
- **JSUI's two patches on top of rnq** still gain about 2% (2.40 → 2.35). rnq doesn't inline the refcount
  helpers or `===` itself, so the two patch sets add up.

## Startup (wall ms, process start to exit, median of 15)

| Build | trivial `print(1)` | load+init bundle (512 KB) | load+init (147 KB minified) | bundle cost over trivial |
|---|---|---|---|---|
| JSUI v0.17.0 patched | 4.6 | 40.0 | 32.8 | 35.4 |
| JSUI v0.17.0 patched, bytecode exe | 4.6 | 6.2 | 6.1 | 1.6 |
| stock v0.17.0 | 4.7 | 39.7 | 32.5 | 35.0 |
| df836d1 | 4.7 | 40.9 | 33.3 | 36.2 |
| rnq | 4.7 | 42.4 | 34.2 | 37.6 |
| rnq + JSUI patches | 4.5 | 41.7 | 34.2 | 37.2 |
| rnq17 + JSUI patches | 4.6 | 41.8 | 33.9 | 37.2 |
| rnq17 + JSUI patches, bytecode exe | 4.8 | 6.7 | 6.8 | 1.9 |

Parsing and compiling source is about 4% slower with rnq (+1.5 ms on 512 KB), probably from the extra
shape-template and fusion passes. Loading bytecode costs +0.3 ms. The bytecode payload grows from 190,249 to
205,598 bytes (+8%, `.qjsexe` minus the trivial one).

## Memory (max RSS, MiB, median of 3)

| Build | trivial | bundle + React init | full bench |
|---|---|---|---|
| JSUI v0.17.0 patched | 2.5 | 8.2 | 13.3 |
| JSUI v0.17.0 patched, bytecode exe | 2.4 | 3.5 | 6.8 |
| stock v0.17.0 | 2.5 | 8.8 | 11.2 |
| df836d1 | 2.5 | 8.1 | 13.3 |
| rnq | 2.6 | 10.0 | 13.5 |
| rnq + JSUI patches | 2.6 | 8.2 | 13.5 |
| rnq17 + JSUI patches | 2.6 | 8.2 | 13.5 |
| rnq17 + JSUI patches, bytecode exe | 2.5 | 3.7 | 7.3 |

The same build moves by up to 2 MiB from run to run here (init 8.1 to 10.0, for example), so treat the full-bench
column as equal for all builds. With bytecode, rnq uses +0.2 to +0.5 MiB (shape templates and inline-cache slots).

## Size (bytes)

| Build | `quickjs.o` file | `quickjs.o` `__text` | `quickjs.o` `__TEXT` total | engine dylib (quickjs+libregexp+libunicode+dtoa), `strip -x` | its `__TEXT` | its gzip -9 | `qjs` exe stripped / `__TEXT` |
|---|---|---|---|---|---|---|---|
| JSUI v0.17.0 patched | 1,171,928 | 791,668 | 826,790 | 1,030,392 | 950,272 | 486,077 | 1,119,008 / 1,032,192 |
| stock v0.17.0 | 1,180,640 | 798,720 | 833,842 | | | | 1,135,496 / 1,048,576 |
| df836d1 | 1,178,840 | 796,972 | 832,024 | 1,030,216 | 950,272 | 487,298 | 1,118,856 / 1,032,192 |
| rnq | 1,284,632 | 880,300 | 917,644 | | | | 1,235,656 / 1,130,496 |
| rnq + JSUI patches | 1,288,744 | 884,932 | 922,276 | 1,130,616 | 1,048,576 | 541,344 | 1,235,664 / 1,130,496 |
| rnq17 + JSUI patches | 1,289,920 | 886,352 | 923,790 | 1,130,728 | 1,048,576 | 542,425 | 1,235,776 / 1,130,496 |

The rnq patches add about 95 KB of code to `quickjs.o` (+12%) and 100 KB to the stripped engine (+56 KB gzipped).
`libregexp.o` grows from 34.9 to 40.3 KB (`__TEXT`); libunicode is unchanged.

## Correctness

quickjs-ng's own suite, `run-test262 -c tests.conf` (what `make test` runs; it provides the `qjs` global and
the per-test expectations), built against each engine:

| Build | tests/ via run-test262 |
|---|---|
| df836d1 (stock tree) | 0/112 errors, 9 excluded |
| rnq / rnq + JSUI patches (df836d1 tests) | 0/112 errors, 9 excluded. `test_bjson.js`'s fuzz case prints "invalid version (27 expected=28)": its corpus is still at the old BC_VERSION in their unpatched submodule |
| **rnq17 + JSUI patches** (v0.17.0 tests, corpus updated by 9999) | **0/116 errors, 9 excluded** |

Every `tests/*.js` was also run as a plain script, `qjs-rnq17-jsui tests/X.js` (86 files). The only failures are
the same six that stock v0.17.0 `qjs` fails the same way: `test_builtin.js` (needs run-test262's `qjs` global),
and bug1354/bug1661/bug648/bug775/bug776, which test for an expected error. `test_language.js`, `test_std.js`,
`test_bigint.js`, `test_bjson.js`, `test_closure.js`, `test_loop.js`, `test_worker.js` and the rest pass. The df836d1-based `qjs-rnq-jsui` gives
the same output as `qjs-df836d1`, except for the test_bjson corpus version above. test262 itself was not run.

The JSUI commit bench (`packages/react/bench/commit-bench.jsx`) imports `react-native` and needs the native
bridge, so it can't run under plain qjs. `out/react-commit.js` is the plain-JS version of the same scenarios
(the tables above). Running it in `jsui-render` needs a JSUI build with the patched engine. See "In JSUI" below.

## GC pauses: navigation bench (`gc-bench.jsx`)

Every frame pushes a new screen (2,000 host views: 200 rows with `useState`/`useCallback`, 9 items each with an
inline `onPress` closure and a per-item data object). The 3 previous screens stay mounted (a navigation stack,
memoized, so only the screen being hidden re-renders) and the oldest is unmounted. Each screen also has a model
with reference cycles (rows point back at the model, listeners close over it), like app state or a store.
React clears a deleted fiber's own pointers, so without that model refcounting alone frees an unmounted screen.
There are 20 warm-up frames and 300 measured frames. A frame is one `updateContainerSync` + `flushSyncWork`.

QuickJS deferred GC: `qjs-rnq17-jsui-gc` with `QJS_GC_DEFERRED=1` calls `JS_SetGCDeferred(rt, true, limit)` and
defines `__runPendingGC()`, which the bench calls between frames, timed apart from the frame. The limit is
re-derived after every collection the way rnq's runtime does with `deferredGCLimit = 0`:
`live + clamp(live, 48 MiB, 256 MiB)`. The engine default, `threshold * 4` taken when deferral is switched on, is
1 MiB, and with it nothing is ever deferred. Two differences from rnq's runtime: the hook drains at every
frame boundary instead of waiting for 120 ms of idle, and it is called from JS, so there is a stack frame
(the `assert` in `JS_RunPendingGC` is compiled out by `-DNDEBUG`; the collection itself is the same one `std.gc()` runs).

The host-driven rows use `gc-driver.c`, which runs the same bundle frame by frame from C (`__gcFrame(f)`),
so collections can run between frames with an empty JS stack, the way JSUI's runtime does it. `JS_RunIdleGC`
refuses to run under JS. They are built from a JSUI build tree's patched QuickJS (`build/quickjs-patched/quickjs.c` +
`build/quickjs-src`, same flags as the others): main a6c34a4 (v0.17.0 + #93; it has no GC stats API), and
`claude/wip-rnquickjs` 45b7f07 (rnq patches + `third_party/quickjs-jsui/0001-generational-gc.patch` + #93's
string patches). Modes:
- `gen`: `JS_SetGCGenerational(rt, true)` only.
- `gen-deferred`: also `JS_SetGCDeferred(rt, true, 0)`, with one `JS_RunIdleGC` between frames.
- `+ 2 ms young slices`: also `JS_SetGCYoungBudget(rt, 2.0)`.
- `JSUI runtime policy`: the WIP's settings plus `QuickJsEngine::collectIdle` between frames, ported as is. A light frame
  is a tick under 4 ms, and none is idle. Every frame here is heavy (about 27 ms), so a pending young collection
  finishes (`JS_FinishPendingGC`) after 3 frames, and a due full one (`JS_RunGC`) runs after 30.

Hermes has no high-resolution clock in its CLI, so its frame times are whole milliseconds (`Date.now`).
Hermes's own GC stats (`-gc-print-stats`) are given beside them. Median of 3 runs, taken under the shared bench
lock at load 5 to 8 (2026-10-25). The rows match an earlier run within 2%:

| Engine / mode | frame p50 | p95 | p99 | max | between-frame GC (p99 / max / total ms) | collections, GC time, longest pause | max RSS (MiB) |
|---|---|---|---|---|---|---|---|
| QuickJS JSUI v0.17.0 (`qjs`) | 35.7 | 37.0 | 74.2 | 78.4 | | 4 inside frames, 28 to 40 ms each | 81.7 |
| QuickJS rnq17 + JSUI, default GC | 27.0 | 28.0 | 61.5 | 62.9 | | 4 inside frames, 27 to 36 ms each | 83.1 |
| QuickJS rnq17 + JSUI, deferred GC (rnq-style ceiling, `__runPendingGC`) | 27.1 | 28.1 | 29.8 | 31.8 | 21.7 / 22.4 / 210 | about 10 between frames, about 21 ms each | 68.3 |
| host-driven: main (v0.17.0 + #93), default GC | 33.4 | 34.5 | 71.2 | 72.0 | | (no stats API on main) | 78.2 |
| host-driven: WIP 45b7f07, default GC | 26.7 | 27.6 | 60.5 | 62.4 | | 4 full, 136 ms, 35.2 ms | 79.6 |
| host-driven: WIP 45b7f07, `gen` | 26.8 | 28.0 | 53.7 | 54.8 | | 8 young, 200 ms, 27.3 ms (inside frames) | 82.7 |
| host-driven: WIP 45b7f07, `gen-deferred` | 26.7 | 27.6 | 28.0 | 28.1 | 24.8 / 25.4 / 988 | 147 (8 full), 988 ms, 25.4 ms | 67.0 |
| host-driven: WIP 45b7f07, `gen-deferred` + 2 ms young slices | 26.9 | 27.8 | 31.1 | 41.5 | 3.1 / 64.3 / 705 | 302 (2 full), 734 ms, 64.3 ms | 114.8 |
| **host-driven: WIP 45b7f07, JSUI runtime policy (`collectIdle`)** | **26.8** | **27.7** | **29.0** | **30.9** | 24.6 / 27.1 / 631 | 45 (3 full), 631 ms, 27.1 ms | 73.7 |
| Hermes .hbc (interpreter) | 15 | 18 | 19 | 21 | | 822, 0.72 s, max pause 3.6 to 3.8 ms | 69.0 |
| Hermes .hbc + JIT | 10 | 12 | 12 | 13 | | 838, 0.61 s, max pause 2.8 to 2.9 ms | 67.3 |

- QuickJS's refcounting frees almost everything as it goes. With stock timing the cycle collector runs about once
  every 75 frames, and each run is a 27 to 40 ms pause on top of a 27 to 36 ms frame. Any deferral moves it out of the
  frame: in-frame p99 falls from 60 to 62 ms to 28 to 30 ms, and peak RSS is 10 to 15 MiB lower.
- Generational collection doesn't pay off in this bench. Each frame's new screen survives 3 more frames, so
  young collections mostly scan live objects. Total GC time is 3 to 5x the full-only deferred run (631 to 988 ms vs
  136 to 210 ms), and the longest between-frame pause is about the same (25 to 27 ms vs 22 ms). With 2 ms slices and
  only one slice per frame, collection can't keep up with a full screen per frame: RSS reaches 115 MiB and one 64 ms
  catch-up runs. JSUI's policy avoids that by finishing in one go after 3 heavy frames. Real navigation has light
  frames between mounts (see jsui-nav below), which is the case the slices are built for.
- Hermes (Hades, concurrent old-gen marking) collects about 2.6 times per frame, but no pause exceeds 4 ms.
  Its frame times have no GC tail.
- Peak RSS is about the same here (67 to 83 MiB). QuickJS's advantage in the smaller benches comes from its
  small heap and bytecode, not from this kind of churn.

Exploratory, single runs: the first version of the bench didn't memoize the kept screens, so every frame
re-rendered all four mounted screens (8,000 views). QuickJS rnq17: p50 49.2 / p99 85 / max 88 ms.
**Hermes interpreter: p50 135 / p95 244 / p99 302 / max 315 ms, with 36.5 s of its 45.6 s run spent in GC
(2,235 collections, 4.2 GB allocated, average pause 17.6 ms).** The JIT did no better (p50 169). `-gc-init-heap=256M
-gc-max-heap=1G` brought it to p50 100 ms at 223 MiB RSS. When a large retained tree re-renders every frame,
Hermes's generational GC degrades badly and QuickJS doesn't. This is a reason to tune Hades for JSUI-sized heaps
before relying on it.

## Head-to-head: best QuickJS (rnq17 + JSUI patches) vs Hermes

Hermes numbers are from `hermes-v260318099.0.4` (the build in `RESULTS.md`), re-run in this session (`results-hermes-rerun.json`).
Hermes needs `-Xes6-block-scoping` (see `RESULTS.md`). The JIT is not allowed on iOS. It is an option only on
desktop and Android.

| | QuickJS rnq17 + JSUI | Hermes .hbc (interpreter) | Hermes .hbc + JIT |
|---|---|---|---|
| mount 500 views (ms) | 2.83 | 1.45 (1.95x) | 0.95 (3.0x) |
| update plain none / tenth / all | 2.33 / 2.35 / 2.34 | 1.20 / 1.21 / 1.20 (1.95x) | 0.60 / 0.60 / 0.60 (3.9x) |
| update memo none / tenth / all | 1.21 / 1.50 / 2.65 | 0.68 / 0.84 / 1.38 (1.8x) | 0.39 / 0.47 / 0.73 (3.2x) |
| GC bench frame p50 / p99 / max | 26.8 / 29.0 / 30.9 JSUI WIP policy; 27.1 / 29.8 / 31.8 rnq-style deferred; 27.0 / 61.5 / 62.9 default | 15 / 19 / 21 | 10 / 12 / 13 |
| longest GC pause | 35 ms inside a frame (default); 22 to 27 ms between frames (deferred) | 4 ms | 3 ms |
| startup, bytecode: trivial / bundle load+init / cost over trivial (ms) | 4.8 / 6.7 / 1.9 | 7.7 / 8.5 / 0.8 | 8.4 / 8.6 / 0.2 |
| peak RSS, React bench (bytecode) | 7.3 MiB | 35.0 MiB | 37.5 MiB |
| peak RSS, GC bench | 68 to 74 MiB (deferred) / 83 MiB (default) | 69 MiB | 67 MiB |
| engine, stripped (bytes) | 1,130,728 (542 KB gz), compiler included | `libhermesvmlean` 1,872,216 (804 KB gz), no compiler; `libhermesvm` 3,198,496 | lean 2,137,712; full 3,480,432 |
| runtime deps | libSystem only | libc++ (shared on Android unless the app already ships it), CoreFoundation on Apple | same |
| bytecode payload (this bundle) | 205,598 | 252,486 | same |

Summary:
- rnq's patches close about 10 to 20% of the gap. Hermes's interpreter is still about 2x faster than the best
  QuickJS build on React commits (was 2.2x), and the JIT about 3 to 4x.
- On GC behaviour Hermes is clearly better for short pauses (4 ms vs 22 to 35 ms) under heavy navigation churn.
  QuickJS's deferred GC removes the pause from frames, but it still needs an idle slot. Hermes in turn degrades badly
  (GC-bound, 100+ ms frames) when big retained trees re-render every frame, unless its heap is tuned.
- QuickJS stays far smaller in memory for normal UIs (7 MiB vs 35 MiB for the React bench), starts as fast from
  bytecode, is ~0.75 MB smaller stripped (1.13 vs 1.87 MB lean), compiles source at runtime (eval, no build
  step needed), and has no C++ runtime dependency.

## In JSUI: main vs `claude/wip-rnquickjs`

Two worktrees with separate Release build dirs (Ninja, no PGO, `JSUI_QUICKJS_FLAGS` empty, Godot off):
main a6c34a4 (0.5.0; QuickJS v0.17.0 + #93's patches) and the WIP branch with the rnq patches applied by Deps.cmake.
The bundles are identical and built with `--format source` on both, because the CLI's bytecode comes from the
WebAssembly build, which the WIP doesn't rebuild. With bytecode, the WIP runtime would fall back to source anyway.

Found on the way and fixed on the branch (690342b): with the build dir inside a git checkout, `git apply` silently
skipped every patch, so the "WIP" build was stock v0.17.0. With rnq applied, #93's `shape-transitions` string
patch no longer matched; 45b7f07 drops it as superseded by rnq 0051.

### jsui-bench (`tools/jsui-bench/run.sh`, all scenarios), WIP 690342b

Five runs of each build, alternated (main, WIP, main, ...), with the median of each scenario's per-frame mean, in µs.
These ran at load ~6, before the lock rule, while nothing else was building. Every counter except allocs is
identical between the builds.

| scenario | JS mean main | JS mean WIP | Δ JS | total mean main | total mean WIP | Δ total | total p95 main | total p95 WIP |
|---|---|---|---|---|---|---|---|---|
| interactions | 12.9 | 12.3 | n/a | 2091.4 | 2071.4 | -1.0% | 2483.9 | 2501.7 |
| interactions-wheel | 139.8 | 127.7 | -8.6% | 2375.6 | 2363.0 | -0.5% | 3131.0 | 3122.0 |
| list | 181.6 | 162.3 | -10.6% | 269.3 | 245.1 | -9.0% | 357.8 | 354.9 |
| menu | 4.6 | 3.6 | n/a | 443.7 | 439.7 | -0.9% | 579.6 | 570.6 |
| inventory | 1534.6 | 1359.0 | -11.4% | 1832.3 | 1655.6 | -9.6% | 2138.4 | 1955.0 |
| idle | 0.0 | 0.0 | n/a | 0.1 | 0.1 | +4.1% | 0.1 | 0.1 |
| mount | 11064.5 | 8372.4 | -24.3% | 14916.3 | 12189.5 | -18.3% | 28799.0 | 13943.8 |
| surfaces-static | 0.0 | 0.0 | n/a | 1.1 | 1.1 | -1.5% | 1.3 | 1.2 |
| surfaces-props | 1922.8 | 1668.8 | -13.2% | 2329.8 | 2061.3 | -11.5% | 2643.7 | 2349.3 |
| surfaces-same-props | 12.6 | 12.6 | n/a | 13.7 | 13.7 | -0.3% | 14.2 | 14.2 |
| surfaces-one-props | 215.5 | 194.0 | -10.0% | 246.9 | 224.4 | -9.1% | 313.9 | 282.3 |
| surfaces-resize | 1609.8 | 1377.7 | -14.4% | 2138.8 | 1909.9 | -10.7% | 2443.6 | 2192.7 |
| markers-json-30 | 429.2 | 378.9 | -11.7% | 523.6 | 473.4 | -9.6% | 643.8 | 593.8 |
| markers-buffer-30 | 393.0 | 357.6 | -9.0% | 471.0 | 435.5 | -7.5% | 581.7 | 540.1 |
| markers-native-30 | 3.3 | 2.9 | n/a | 20.7 | 20.6 | -0.6% | 21.3 | 21.0 |
| markers-json-100 | 1289.2 | 1145.1 | -11.2% | 1617.3 | 1472.3 | -9.0% | 1854.2 | 1710.1 |
| markers-buffer-100 | 1184.7 | 1078.9 | -8.9% | 1456.6 | 1352.3 | -7.2% | 1693.4 | 1587.3 |
| markers-native-100 | 3.5 | 3.1 | n/a | 60.6 | 60.3 | -0.5% | 63.8 | 65.0 |
| markers-json-1000 | 12799.7 | 11433.2 | -10.7% | 16255.4 | 14946.1 | -8.1% | 17014.0 | 15702.7 |
| markers-buffer-1000 | 11775.9 | 10828.5 | -8.0% | 14670.9 | 13859.2 | -5.5% | 15317.7 | 14585.2 |
| markers-native-1000 | 10.0 | 9.3 | n/a | 862.8 | 863.3 | +0.1% | 1129.0 | 1156.5 |

- JS time is 8 to 14% lower in every JS-bound scenario, and 24% lower for `mount` (11.1 → 8.4 ms). Frame totals are
  5.5 to 11.5% lower. The `mount` total falls 18%, and its p95 falls from 28.8 to 13.9 ms, because the cycle collection
  no longer lands inside a mount. Paint-bound and idle scenarios are unchanged (within ±1%).
- Peak RSS for the whole suite: main 289.7 to 293.9 MiB, WIP 314.8 to 327.2 MiB (+25 to 35 MiB, 690342b's GC policy).
  The 45b7f07 mount runs below look at this per variant.

| | main | WIP |
|---|---|---|
| `libjsui_quickjs.a` | 1,321,224 | 1,460,304 |
| `quickjs.c.o` file / `__text` / `__TEXT` | 1,173,272 / 792,588 / 827,710 | 1,300,392 / 895,232 / 932,670 |
| archive `__TEXT` total (quickjs + libregexp + libunicode + dtoa) | 937,006 | 1,047,779 |
| `jsui-tests` | 253 tests, 0 failed | 254 tests, 0 failed (690342b, 39e1b82 and 45b7f07) |

### Showcase navigation (`jsui-nav`) and GC variants, WIP 45b7f07

`jsui-nav` (from the WIP's `tools/jsui-bench/nav.cpp`; main's copy has the `gc_*` printf `#ifdef`ed out) loads the
site bundle (`node packages/cli/bin/jsui.js build site/app/index.jsx --minify --format source`). It ticks 240
warm-up frames, then runs 3 rounds of 8 route changes with 200 tick + incremental 1280x800 CPU renders each
(4,800 frames). Variants of the WIP are local, uncommitted edits to the `QuickJsEngine` constructor:
- (a) as is: generational + deferred, collectIdle.
- (b) `JS_SetGCGenerational(impl.jrt, false)`: deferred full collections only.
- (c) both lines removed: stock GC timing on the patched engine.

Five rounds of main/a/b/c with the order rotated each round, under the bench lock at load 5 to 8 (up to 10.8 in
the last round). Medians:

| Build | p50 | p95 | p99 | max | frames > 16.7 ms | gc (collections, full, total, longest) | peak RSS (MiB) |
|---|---|---|---|---|---|---|---|
| main | 2.76 | 8.03 | 10.42 | 27.6 | 21 | (no stats) | 264.1 |
| WIP (a) generational + deferred | 2.79 | 8.05 | 10.54 | 26.3 | 21 | 23 (2 full), 45.7 ms, 8.0 ms | 257.5 |
| WIP (b) deferred, full only | 2.75 | 7.96 | 10.52 | 26.7 | 21 | 6 (6 full), 31.1 ms, 7.0 ms | 260.6 |
| WIP (c) stock GC timing | 2.77 | 8.05 | 10.36 | 27.3 | 20 | 4 (4 full), 31.1 ms, 12.6 ms | 267.7 |

Max across runs was 27.2 to 28.5 ms for main, 25.5 to 27.7 for (b) and (c), and 25.5 to 27.6 for (a), plus one
133 ms frame in its first run.
Showcase navigation is paint/layout-bound at this size: the engine and GC policy don't move the frame
distribution, and GC is 31 to 46 ms over 4,800 frames. Stock timing (c) gives the longest single pause (12.6 ms),
deferral halves it, and generational (a) spends 50% more GC time in more, smaller collections. An earlier set of runs
(690342b, during another session's emscripten build at load 24 to 29) showed 90 to 196 ms tails on both builds.
Those are superseded by these.

`jsui-bench mount.js --frames 300` (`tools/jsui-bench/scenarios/mount.jsx`, `--format source`), median of 3:

| Build | peak RSS (MiB) | JS mean / p50 / p95 (ms) | total mean / p50 / p95 (ms) |
|---|---|---|---|
| main | 100.8 | 10.37 / 8.98 / 24.04 | 14.15 / 12.71 / 27.91 |
| WIP (a) generational + deferred | 105.8 | 7.74 / 7.76 / 8.61 | 11.47 / 11.44 / 12.79 |
| WIP (b) deferred, full only | 100.6 | 7.60 / 7.59 / 8.41 | 11.28 / 11.26 / 12.44 |
| WIP (c) stock GC timing | 99.8 | 8.99 / 7.75 / 20.86 | 12.74 / 11.45 / 24.57 |

The p95 spike on main and (c) is the cycle collection inside a mount. Deferring it (a, b) removes it. (b) is
slightly faster than (a) and uses 5 MiB less, so on these two workloads deferral alone gets the whole benefit
and generational adds GC work and memory.

## Reproduce

Sources are under `~/Development/engine-bench/src` (`rnquickjs` at c8102d4 with the `engine/quickjs-ng` submodule;
`quickjs-ng-0.17.0`; `quickjs-rnq17` = v0.17.0 + `git am third_party/quickjs-rnq/*.patch`). Objects and
patched copies are under `qjs-build/<name>/`, and the binaries are in `bin/`. JSUI's patch is applied with
`cmake -DIN=quickjs.c -DOUT=out/quickjs.c -P jsui-patch.cmake` (the Deps.cmake block, verbatim). In `run/`,
`*.rnq17.qjsexe` = `qjs-rnq17-jsui -c`, `gc.js` = `out/gc-bench.js`, and `gc.hbc` = `hermesc -O -Xes6-block-scoping -emit-binary`.
Then run `python3 bench/engines/run.py QuickJS` and `python3 bench/engines/gc-run.py`. `gc-driver.c` is built from a JSUI build
dir: `cc <flags> -DHAVE_YOUNG_BUDGET -DHAVE_JSUI_POLICY -I build/quickjs-src build/quickjs-patched/quickjs.c build/quickjs-src/{libregexp,libunicode,dtoa}.c gc-driver.c`
(main: without those defines, with the GC calls removed). `qjs-gc-hook.patch` is the `__runPendingGC` hook in `qjs.c`, and
`build-qjs.sh` / `jsui-patch.cmake` build the qjs binaries. jsui-bench raw JSON is in `jsui-ab/`.
