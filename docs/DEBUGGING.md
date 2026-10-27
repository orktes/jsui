# Debugging JS

Set breakpoints in your JSX/TSX, step through it, look at variables and
evaluate expressions in the running app from **Chrome DevTools** or
**VS Code**. This works in every native runtime: the viewer, Godot, Unity,
Unreal, the engine examples, Android and iOS.

```sh
npx jsui dev src/App.jsx        # the dev server (debugging is on by default)
jsui-viewer --dev http://127.0.0.1:5173/bundle.js   # or any dev-mode host
```

* **Chrome:** open `chrome://inspect`, click *Configure...* and add
  `127.0.0.1:5173`. The app shows under *Remote Target*. Click *inspect*.
  If it doesn't show up there, open `http://127.0.0.1:5173/json/list` and
  paste the runtime's `devtoolsFrontendUrl` (`devtools://devtools/bundled/js_app.html?…`)
  into the address bar. The id in it changes each time the runtime starts.
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
* **Not supported.** Profiling, heap snapshots, live edit and restart frame.
  The dev server answers the `Profiler`, `HeapProfiler` and `Network`
  domains with "not found", so DevTools doesn't wait for a profile that
  never comes. For where JS time goes, run `tools/jsui-bench` with
  `JSUI_JS_PROFILE=out.txt` (see [PERFORMANCE.md](PERFORMANCE.md)).
* **Chrome.** It loads the source maps through `chrome://inspect`'s DevTools.
  A DevTools frontend hosted on an http page can't fetch them, because of its
  content security policy.
