# quickjs-cdp

The Chrome DevTools Protocol agent for QuickJS from
[react-native-quickjs](https://github.com/react-native-quickjs/quickjs)
(`modules/cdp`, commit c8102d4, MIT, see LICENSE), used by JSUI's JS
debugger (`src/js/inspector.cpp`, `docs/DEBUGGING.md`). It needs the engine
patches 0003-0005 in `../quickjs-rnq`.

JSUI's changes are marked `jsui:` in the source:

* **Windows.** Locks and condition variables use SRW locks on Windows and
  pthreads elsewhere.
* **`Debugger.scriptParsed`.** It reports `sourceMapURL` and `hasSourceURL`
  from a script's trailing comments.
* **Breakpoints.**
  * `Debugger.setBreakpointByUrl` takes `urlRegex` (VS Code uses it).
  * `Debugger.setBreakpoint` and `Debugger.getPossibleBreakpoints` are
    implemented.
  * Breakpoint conditions are evaluated.
  * A breakpoint stops once per visit of its line.
  * The per-statement trap first checks whether any breakpoint sits on that
    line.
  * `Debugger.setSkipAllPauses` is implemented.
* **`Runtime.callFunctionOn`** is implemented. Both frontends use it to
  preview and expand values.
* **`returnByValue`** works in `Runtime.evaluate` and
  `Debugger.evaluateOnCallFrame`.
* **`Runtime.getProperties`.**
  * It takes `accessorPropertiesOnly`.
  * It reports `[[Prototype]]`.
  * Children go into their parent's object group.
* **Paused frames.**
  * Each one gets a global scope.
  * Frames of scripts the frontend never saw (the runtime's prelude) are left
    out.
  * The engine's hidden temporaries are left out of local scopes.
  * The scope objects are released on resume.
* **Descriptions.** Objects are described as V8 does them (`Object`,
  `Array(3)`).
* **`qjs_cdp_exception_thrown`** reports uncaught errors as
  `Runtime.exceptionThrown`.
* **`qjs_cdp_pause`** only sets a flag, so it is safe from any thread.
