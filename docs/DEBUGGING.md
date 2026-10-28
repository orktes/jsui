# Debugging JS

Set breakpoints in your JSX/TSX, step through it, look at variables and
evaluate expressions in the running app from **Chrome DevTools** or
**VS Code**. This works in every native runtime: the viewer, Godot, Unity,
Unreal, the engine examples, Android and iOS.

```sh
npx jsui dev src/App.jsx        # the dev server (debugging is on by default)
jsui-viewer --dev http://127.0.0.1:5173/bundle.js   # or any dev-mode host
```

* **Chrome:** open `http://127.0.0.1:5173/__jsui/devtools/` in a tab.
  It is DevTools on the runtime that joined last, and the same link works
  after the app or the server restarts. It opens the runtime as a page
  would be (see Profiling: the Performance panel records a track per
  thread). The frontend is Chrome's prebuilt one for a pinned revision
  (`DEVTOOLS_REVISION` in `packages/cli/src/devtools-frontend.js`,
  BSD-licensed), fetched from Google's DevTools CDN on first use and cached
  in `~/.cache/jsui/devtools/` (`JSUI_CACHE_DIR` moves it).
* **Chrome's own DevTools** (works offline): paste
  `devtools://devtools/bundled/inspector.html?ws=127.0.0.1:5173/__jsui/inspect/page/<id>`
  into the address bar, the same page mode. `jsui dev` prints it with the
  runtime's id when one connects (`/json/list` has it as
  `jsuiPageFrontendUrl`). Or the Node-style way: `chrome://inspect`,
  *Configure...*, add `127.0.0.1:5173`, *inspect* the app under *Remote
  Target* (or its `devtoolsFrontendUrl`). That one records only the CPU
  profile. The id changes each time the runtime starts.
* **VS Code**: add this to `.vscode/launch.json` and start *JSUI*:

  ```json
  { "type": "node", "request": "attach", "name": "JSUI",
    "address": "127.0.0.1", "port": 5173, "resolveSourceMapLocations": null }
  ```

  `resolveSourceMapLocations: null` lets VS Code read the source maps the dev
  server serves over http. By default it only reads maps from the workspace.

Breakpoints, conditional breakpoints, `debugger;`, step over/into/out, the
call stack (mapped back to your files and to React's), local and global
scopes, editing a local, hover and console evaluation in the paused frame,
and `console.*` with live objects all work.

`jsui dev` prints both snippets when it starts. It logs when a runtime or a
debugger connects. `jsui dev --no-inspect` turns debugging off.

## Profiling

DevTools' **Performance** panel records a CPU profile of the app's JS: press
record, use the app, press stop. The flame chart shows your functions with
their names and files, through the source map, and `(idle)` where JS wasn't
running. *Save profile* writes a `.cpuprofile`.

* **What it measures.** Time on the JS thread while JS runs, by JS function.
  Time in a native call (a host call, `JSON.parse`) counts toward the JS
  function that made it.
* **What called into JS.** Each stretch of JS sits under what the runtime
  called it for: `(timer)`, `(animation frame)`, `(event: pointerup)`,
  `(host message: name)`, `(call: name)` (a global the runtime calls, such
  as `jsui dev`'s own `__jsui_onCall`), `(host call reply)`, `(measure)`,
  `(onLayout)`, `(script: url)`, and `(microtasks)` for the promise jobs
  that run after them. In the trace they are Chrome's own Task, Timer
  fired, Animation frame fired, Event and Function call.
* **The runtime's own work.** Where JS was idle, the frame phases the
  runtime ran show as top-level entries: `(commit)`, `(style)`, `(layout)`,
  `(animated)`, `(input)`, `(paint)`, `(worklets)` (react-native-worklets
  runs, as one entry: what runs inside them isn't sampled yet) and `(gpu)`
  (submitting the frame to the GPU, not GPU execution). They run on the window's thread: with JS on
  its own thread (the default), a phase that overlaps running JS shows only
  where JS was idle. With `--js-thread off` everything is one thread and
  the chart shows whole frames. Totals per phase without DevTools:
  `jsui_set_profiling` / `getPerf()` (see [PERFORMANCE.md](PERFORMANCE.md)).
* **A track per thread.** Opened as a page (the links above), DevTools
  records a trace (CDP `Tracing`): the dev server answers it by recording
  a CPU profile in the runtime and sending it back as trace events. The
  recording then shows the way Chrome shows a page's: the JS on **Main**
  (in tasks, under what called it), the frame phases on a **JSUI render
  thread** track (Layout, Recalculate style, Paint, Commit, colored as
  Rendering and Painting) where they actually ran, overlapping JS or not,
  worklet runs (react-native-worklets, on the window's thread) on a
  **JSUI → Worklets** track, and each GPU submit on the **GPU** track. Opened the
  Node-style way (`chrome://inspect`), DevTools asks for a CPU profile only
  and shows one lane, with the phases as `(layout)`, `(paint)`, ... entries.
* **How.** A thread of the runtime's arms the statement traps once per
  sampling interval (`Profiler.setSamplingInterval`, 1 ms unless the
  frontend asks otherwise, at least 100 µs), and the next
  statement records the JS stack. Between samples nothing runs: a JS
  benchmark ran 2% slower while recording at 100 µs (M1 Ultra). A function shorter than the interval can
  fall between samples: profile long enough, or repeat the interaction.
* **Where.** Every native runtime in `jsui dev`. The browser's own engine
  has its own profiler; the web build's QuickJS and the headless runtime
  can't be debugged at all (see below).

## Elements

Opened as a page (the links above), the **Elements** panel shows the
runtime's view tree: `View`, `Text`, `Image`, `ScrollView`, ... under
`AppRoot`, with `testID`, `nativeID`, `accessibilityLabel` and the like as
attributes, the view's style as its `style` attribute, and string children
as text nodes. It follows the app a few times a second (views added and
removed, props and text changing).

* Hovering a node highlights its view in the app; the box model (size,
  padding, border, margin) comes from layout.
* The Styles pane shows the view's style as `element.style`. Edit a value,
  add a property or untick one and the view changes at once. Values are
  React Native's: `backgroundColor: #ff0000`, `width: 120`, `transform:
  [{"scale":1.2}]` (JSON for arrays and objects).
* Double-click a text node to change the text, or an attribute to change
  the prop.
* React owns these props: the next time the component renders, it writes
  its own values back over an edit, as React DOM does in a browser.
* Not there yet: picking a view in the app (the inspect arrow), `$0` in
  the console, and the React component names around the views.

## Network

The **Network** panel lists what the runtime loads while DevTools is open:
`fetch()` calls, images, fonts, the bundle and hot updates, with method,
headers, status, type, size, timing and the response (the last bodies, up
to 16 MB each and 96 MB in all, stay readable). The dev server's own
traffic (`/__jsui/...`: the shared session, the debugger) is left out.
Requests show from the moment DevTools enables the panel; there is no
"reload page". WebSockets aren't shown yet.

## How it works

```
Chrome DevTools / VS Code ──WebSocket──> jsui dev ──HTTP long-poll──> runtime (QuickJS + CDP agent)
          (/json/list, ws://…/__jsui/inspect/ws/<id>)     (/__jsui/inspect/poll, /push)
```

* **The engine.** The runtime's QuickJS carries three engine patches from
  [react-native-quickjs](https://github.com/react-native-quickjs/quickjs)
  (`third_party/quickjs-rnq/0003-0005`). They add a per-statement debug trap,
  stack and local inspection, and evaluation in a frame. The parser emits the
  traps only into code it parses while a debugger is attached, so code loaded
  without one runs exactly as before.
* **The agent.** Their Chrome DevTools Protocol agent
  (`third_party/quickjs-cdp`) implements `Debugger` and `Runtime`. It is
  extended for VS Code and source maps; the changes are listed in its
  README.
* **The runtime** (`src/js/inspector.cpp`). A dev-mode runtime whose bundle
  comes from a dev server that sends `X-Jsui-Inspect: 1` attaches the agent to
  each new JS context before the bundle is parsed. CDP travels through the dev
  server over HTTP, on two threads of the inspector's own. Engines need no
  sockets, and a phone on the LAN debugs the same way. Those threads keep
  answering while JS is paused at a breakpoint and the runtime's own loop is
  blocked.
* **The dev server** (`packages/cli/src/inspect.js`). It lists every connected
  runtime as a target in `/json/list`, the same way Node does, and relays each
  frontend's WebSocket to it. The bundle and every hot update come with
  source maps to your files on disk.
* **Reloads and hot updates.** A debugger stays attached across reloads and
  hot updates. Breakpoints carry over to the new JS context, and each hot
  update is a script of its own (`/__jsui/hmr/<version>.js`).
* **When the debugger closes.** Closing the last debugger resumes the app and
  clears its breakpoints.

## The browser

The web build running on the browser's own JS engine (`createJsui`'s
default `js: 'host'`) needs nothing from JSUI. Open the browser's DevTools
on the page and debug the bundle like any script. `jsui dev` serves source
maps, so the Sources panel shows your files. The web build's QuickJS
(`js: 'quickjs'`) cannot pause, because WebAssembly on the page's thread can't
block, so switch to the host engine to debug.

The headless runtime (`jsui dev --headless`, `jsui screenshot`, the
WebAssembly module in Node) is not a debug target for the same reason.

## Cost

* **Build flag.** The debugger is compiled in when CMake's `JSUI_DEBUGGER`
  is on. It defaults to on for native builds and off for Emscripten.
  `-DJSUI_DEBUGGER=OFF` compiles out the engine hooks, the agent and the
  transport. Without it, the opcode numbering and the bytecode format stay
  the same.
* **Size.** The debugger adds about 94 KB to the stripped Linux `libjsui.so`.
* **Without a dev server.** A runtime that doesn't load from a debugging dev
  server never attaches the agent. Its code has no traps and runs at full
  speed. Precompiled bundles (`jsui build`) never contain traps.
* **In `jsui dev`.** Each statement runs one extra trap instruction, which
  checks a flag. The traps stay off while nothing can stop at them, so an
  attached debugger with no breakpoints costs about the same. While a
  breakpoint is set, or while stepping, each statement also looks up its
  line: CPU-heavy JS runs about 2x slower until you remove the breakpoints.

## Limits

* **Breakpoints bind to lines.** A breakpoint stops at the first statement on
  its line. Other statements on that line in the same call, such as a
  callback written on the same line, don't stop again.
* **Pause on exceptions.** It isn't supported, because the engine has no throw
  hook yet. Uncaught errors still show in the console with their stack.
* **Code that already ran.** A breakpoint in code that ran before the debugger
  attached, such as module top level on first load, can't stop until that code
  runs again. Set the breakpoint and reload the app: the viewer's *R*, or save
  a file.
* **The app freezes while paused.** It is paused like a browser tab. In a game
  engine, the engine's main loop waits with it.
* **Not supported.** Heap snapshots (the Memory panel), live edit and
  restart frame. The runtime answers those domains with "not found".
* **Chrome.** It loads the source maps through `chrome://inspect`'s DevTools.
  A DevTools frontend hosted on an http page can't fetch them, because of its
  content security policy.
