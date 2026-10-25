# Performance: what JSUI costs your game

JSUI is a UI layer inside someone else's frame. What matters is how much of
the game's frame time it takes, whether it can ever make a frame late, and
how much memory and disk it adds. This page answers those with measured
numbers.

Unless a section says otherwise, numbers come from a Mac Studio (M1 Ultra)
on release builds, measured in October 2026 (JSUI 0.6 to 0.8). How to write
UI that keeps these numbers low is in [NATIVE_FIRST.md](NATIVE_FIRST.md);
how the runtime works inside is in
[ARCHITECTURE.md](ARCHITECTURE.md#performance).

## At a glance

| Cost to the game | Measured |
|---|---|
| Frame time added by a moving game HUD, Unity / Unreal (Metal) | **+0.04 / +0.08 ms** ([all engines](#frame-time-in-game-engines)) |
| JS per frame, demo game HUD at rest (1080p) | **5 µs** (0.03% of a 16.67 ms frame) |
| JS per frame, every showcase game screen at rest | **~0** (JS runs only on discrete events) |
| Long React work (screen mount, GC) on a native host | **not on the frame**: JS runs on its own thread |
| UI following 100 moving units | **5 µs** of JS per frame with native Animated |
| Memory, demo HUD | **1.9 MB** JS heap, 24 MB process RSS without painting |
| Disk, `libjsui` linux-x64 | **15.8 MB** |
| Worst first-launch frame (cold Metal shader cache) | **~200 ms** once at startup, then no frame over 30 ms |

## Frame time in game engines

The same game with the UI off and on, measured in each engine example on a
Mac Studio (M1 Ultra, macOS 26), vsync off. Each number is the median of 3
interleaved 8 s rounds' mean frame time, after a warm-up.

| Engine | How JSUI draws | UI off | UI on | Added per frame |
|---|---|---|---|---|
| Unity 6.3 | Skia Graphite on Unity's Metal device, into a RenderTexture | 0.293 ms (3413 fps) | 0.334 ms (2994 fps) | **+0.04 ms** |
| Unreal 5.8 (1280x720, exclusive fullscreen) | Skia Graphite on Unreal's Metal device, into a RenderTarget2D | 2.94 ms (340 fps) | 3.02 ms (332 fps) | **+0.08 ms** |
| Godot (godot-aw3, Forward+ Metal) | Skia Graphite on Godot's Metal device | 3.96 ms (252 fps) | 4.45 ms (225 fps) | **+0.48 ms** (rounds: +0.01 to +0.67) |
| raylib 6.2 (OpenGL) | Skia Ganesh GL into a RenderTexture2D | 0.45 ms (2224 fps) | 0.83 ms (1199 fps) | **+0.38 ms** |
| sokol (OpenGL) | Skia Ganesh GL into the swapchain | 0.78 ms\* | 1.44 ms\* | **+0.66 ms** |

What the scenes do:

- **Unity, Unreal, raylib, sokol** run the same HUD
  (`integrations/shared/ui/app.jsx`) over 8-9 orbiting units, with
  nameplates following the units every frame through a host buffer. The UI
  repaints every frame, so this is a moving-HUD worst case, not an idle
  screen.
- **Godot** runs its own demo (the showcase UI over a 3D world), mostly idle
  apart from stats updating once a second. Its delta is noisy on a 3.9 ms
  frame (p99 is ~15 ms with or without the UI).

How to read it:

- On the Metal Graphite paths that Unity and Unreal use, a full moving HUD
  costs the game **0.04-0.08 ms per frame**: under 0.5% of a 60 fps frame.
- The OpenGL examples cost more (0.4-0.7 ms). On macOS, OpenGL runs on
  Apple's GL-on-Metal layer, and these examples composite a full-frame UI
  texture every frame. We expect most of that cost to come from that layer,
  but haven't confirmed it.
- Running JS on its own thread or on the main thread made no measurable
  difference here. The HUD's JS is idle between state updates; the
  per-frame work (nameplates, paint) is native either way. The JS thread
  matters for spikes (below), not for the steady frame.

\* macOS paces sokol's GL view at the display refresh whatever the swap
interval, so sokol's numbers are each frame's work (CPU plus `glFinish`),
not frame-to-frame time.

## Where JSUI runs in your frame

On native hosts (Godot, Unity, Unreal, the raylib/SDL3/sokol examples, iOS,
Android, the desktop viewer) a frame of UI touches the game in three places:

1. **Host pushes** (game thread): `jsui_set_buffer` / `jsui_set_state` copy
   your data into the runtime. Microseconds.
2. **Tick** (the thread the host calls it on, usually render or game
   thread): input, native animations, style, layout and paint recording.
   This is JSUI's per-frame cost, and it is proportional to what changed:
   a static HUD does almost nothing, and a frame with nothing new paints
   nothing.
3. **GPU work**: JSUI draws into a texture or straight into the engine's
   render target with Skia Graphite (Metal, Vulkan, D3D12) on the engine's
   own GPU device, incrementally (only the damaged region is redrawn).

**JS is not in that list.** React runs on its own thread, like React
Native's JS thread. The tick draws from the last committed UI plus native
state, so a long commit (opening a big menu), a GC pause or a slow event
handler delays when that change *appears*, but never delays the game's
frame. If a host needs this frame's JS result (for example, it just set
state and wants the UI to reflect it in the same frame), it can call
`jsui_sync(rt, timeout_ms)` to wait for JS explicitly.

## JS cost per frame

Even though JS is off the frame, it still uses a core. A typical game HUD
uses almost none of it. The Unity and Unreal demo HUD
(`integrations/shared/ui/app.jsx`), 1920x1080, with the host pushing an
entity buffer every frame and game state when it changes, exactly as the
Unity example does (µs per frame, mean / p95):

| Scenario | JS | Tick before paint | JS share of 16.67 ms |
|---|---|---|---|
| Idle (entity buffer pushed every frame) | **5.1** / 8.3 | 7.0 / 7.3 | 0.03% |
| Mouse sweeping over buttons | **9.7** / 22.7 | 16 / 47 | 0.06% |
| 48 units with nameplates | **10.8** / 21.5 | 50 / 53 | 0.06% |
| No host pushes | 0.1 | 0.2 | 0% |

Across the showcase's game screens (main menu, HUD, inventory, dialogue,
scoreboard, loading, notifications), per-frame JS at rest is ~0. Loading,
scoreboard and notifications run 1-6 ms of JS on discrete events, which the
JS thread keeps off the frame.

### Why this stays low: native-first UI

Per-frame JS is a choice the UI author makes. The same 100 nameplates
following game units (1280x720, µs of JS per frame, mean / p95):

| Driven by | JS per frame | React renders per frame |
|---|---|---|
| Native Animated / host buffer | **4.8** / 7.6 | 0 |
| React state, from a host buffer | 1232 / 1381 | 101 |
| React state, from JSON game state | 1321 / 1450 | 101 (plus 1-2 ms GC pauses) |

React state costs about 12-14 µs of JS per moving item per frame. Native
Animated values, `Animated.derive` expressions, host-state binds, state
styles (`hoveredStyle`, `pressedStyle`), `<Visibility>` and `<Overlay>` are
evaluated natively every frame from a description React committed once, so
they cost the game nothing in JS, keep moving while JS is busy, and react on
the same frame as the input. [NATIVE_FIRST.md](NATIVE_FIRST.md) says which
one to use when.

The other common trap is a component that re-renders on every game-state
push. The demo HUD originally re-rendered all ~72 of its components
whenever the FPS label changed: 1.1 ms of JS per change. Memoizing the
static parts brought it to 0.4 ms; drawing the label with `Animated.Text`
from a host-state bind takes it to zero.

## Spikes

What can make a frame long, and what JSUI does about it:

| Source | Effect on the game's frame |
|---|---|
| React commit (opening a screen, a big list) | None on native hosts (JS thread). In single-threaded hosts (web), React renders in ~5 ms slices across frames (`jsui_set_js_frame_budget`, default 5 ms); only the final commit is one piece. |
| JS garbage collection | None on native hosts. Full collections are deferred to idle time between frames; an idle HUD makes no garbage and collects nothing. |
| GPU pipeline compiles on first use | Once per pipeline per machine. JSUI compiles on a background pool, precompiles its common pipelines before the first frame (`jsui_gpu_prepare_target`), and keeps a cache file of the pipelines used so later launches build them before the frames need them. What a later launch's rebuild costs depends on the backend: Metal finds them in the system's shader cache; Vulkan in the driver's own pipeline cache, which JSUI saves to disk per GPU and driver (a driver update starts it over); Dawn, GL and Direct3D in the driver's or JSUI's shader cache. |
| Text shaping for a new screen | Done in spare time before the commit when the screen renders in slices. |

First launch on a cold Metal shader cache, navigating the showcase:

| | Worst startup frame | Navigation frames over 30 ms |
|---|---|---|
| Without background compiles or cache | 1080 ms | 36 (3.3 s total) |
| JSUI today, first launch | ~200 ms | 0 |
| JSUI today, second launch | lower again (cache file) | 0 |

In Godot (godot-aw3, Metal), the worst frame of the first UI scroll on a
cold cache is 5.4 ms.

## Memory and disk

**Memory**: the demo HUD's QuickJS heap is 1.9 MB with 8 units and 2.5 MB
with 48, flat with no collections over 1200 idle or hover frames. Process
RSS of the whole runtime is 24 MB without painting. Bundles ship as
precompiled bytecode by default, which loads in 3.7 ms (the shared demo
app) and saves 1.5-2 MB of RSS over source.

**Disk** (release downloads, MB):

| Package | Size |
|---|---|
| `libjsui` linux-x64 | 15.8 |
| `libjsui` macOS (universal: arm64 + x86_64) | 27.1 |
| Godot addon | 37.3 |
| Unity package | 30.7 |
| Unreal plugin | 29.5 |
| iOS xcframework | 43.7 |

One platform slice is ~13.4 MB: Skia 4.7, the built-in Inter fonts 2.1,
ICU line-breaking data 1.7, QuickJS 1.0, HarfBuzz 0.74, Lottie (Skottie)
0.43, miniaudio 0.38.

JSUI uses QuickJS rather than a faster engine on purpose. Hermes would make
React commits about 2x faster but adds ~0.8 MB and 20-28 MB of RAM;
JavaScriptCore (JIT) is 14x faster at commits but uses ~147 MiB. Since game
UI frames run almost no JS and commits are off the frame, the smaller
engine is the better trade for a game.

## Web

On the web, JSUI renders a page's UI into a canvas with WebGPU (falling
back to WebGL, then CPU) and runs the bundle on the browser's own JS
engine. Against Chrome rendering the same React app with react-native-web
(1280x720, Chrome 155 on an M1 Ultra's GPU, main-thread ms per frame,
median of 3 runs):

| App | JSUI | JSUI on QuickJS | Chrome (react-native-web) |
|---|---|---|---|
| 100 moving markers | 1.90 | 3.29 | 1.86 |
| 300 moving markers | 4.48 | 8.49 | 4.59 |
| Inventory screen | 1.17 | 3.18 | 1.06 |

Total CPU across Chrome's processes is within ±10% of Chrome's own
rendering. With `createJsui({ worker: true })` the runtime moves to a
worker and the page's main thread spends 0.17 ms per frame (Chrome: 0.89).
Download (WebGPU build, gzip): 5.4 MB of WebAssembly.

`js: 'quickjs'` (and pages whose Content-Security-Policy blocks `eval`,
which fall back to it) runs the bundle on QuickJS compiled into the
WebAssembly instead: the same UI, but 7 to 20 times the JS time per frame,
which makes UIs that re-render every frame cost two to three times as
much on the main thread. UIs driven by native animation barely notice
(the showcase's interactions screen: 0.76 ms either way, Chrome 0.63
ms). The apps are in
`web/test/bench-apps/`; `node web/test/frame-bench.mjs --real-gpu
--renderers webgpu,chrome [--js quickjs] web/test/bench-apps/markers.jsx`
measures one.

## Measuring your own UI

- **In your game**: `jsui_set_profiling(rt, true)` and `jsui_get_perf`
  (Godot: `profiling` / `get_perf()`) give per-phase times (JS, commit,
  Animated, style, layout, input, paint, GPU submit) and work counters
  (layout passes, text shaping, pipelines compiled, ...). Compare a frame
  with the UI hidden against one with it shown.
- **Where JS time goes**: run with `JSUI_JS_PROFILE=out.txt` for a sampling
  profile by JS function.
- **`tools/jsui-bench`**: runs a bundle on a simulated 60 Hz clock and
  prints mean / p50 / p95 per phase and allocations per frame, with mouse,
  wheel and host-state simulation. Measure JS with the JS thread off, since
  with it on the JS time is on the other thread.
- **`web/test/frame-bench.mjs`**: the web host in real Chrome, including
  Chrome's per-process busy time, with Chrome rendering the same app as a
  baseline.

If JS shows up every frame, look for an animation driven by `setState`, a
timer or rAF loop, or a component that re-renders on every game-state push.
