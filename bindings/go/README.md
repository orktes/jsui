# JSUI for Go

```go
import "github.com/orktes/jsui/bindings/go" // package jsui

runtime.LockOSThread() // GL, the window and the runtime stay on one thread
rt, err := jsui.New(jsui.Config{Width: 900, Height: 760, PixelRatio: 2})
rt.OnCall = func(c jsui.Call) {
	var args struct{ Title string }
	if err := c.Decode(&args); err != nil {
		rt.RejectCall(c.ID, err.Error())
		return
	}
	rt.ResolveCallValue(c.ID, map[string]any{"id": app.Add(args.Title)})
}
rt.OnMessage = func(name, payload string) { log.Println(name, payload) }
rt.SetStateValue("player", Player{Level: 3, XP: 120}) // useHostState('player')
rt.LoadBundleFile("bindings/ui/dist/bundle.js")

gpu, _ := jsui.NewGPUContextGL() // the window's GL context is current
for !win.ShouldClose() {
	glfw.PollEvents() // callbacks: rt.SendMouseMove / SendMouseButton / SendKey / SendText ...
	rt.Tick(jsui.NowMs())
	if rt.NeedsRedraw() {
		rt.RenderGL(gpu, 0, fbWidth, fbHeight, 0) // Skia on the GPU, into the window
		win.SwapBuffers()
	}
}
```

* Handlers are plain fields (`OnMessage`, `OnCall`, `OnCallCancel`,
  `OnState`, `OnLog`), set any time. C reaches them through exported Go
  functions and a `runtime/cgo.Handle` (`shim.c`, `callbacks.go`).
* `SetStateValue`, `PostMessageValue`, `ResolveCallValue` and `Call.Decode`
  marshal with `encoding/json`; the plain variants take JSON text.
* `NewGPUContextGL` and `RenderGL` draw with Skia on the program's OpenGL
  context; `RenderRGBA` / `RenderPNG` on the CPU.
* `include/jsui/jsui.h` is a copy of the repository's header (so the module
  builds on its own); CI checks they match.

Linking: `libjsui` is looked up in the repository's `build/` directory
(built with `-DJSUI_BUILD_SHARED=ON`), which also goes into the rpath. For a
release archive or another location:

```sh
export CGO_LDFLAGS="-L/path/to/jsui/lib -Wl,-rpath,/path/to/jsui/lib"
```

```sh
go test ./...                                         # needs bindings/ui/dist/bundle.js
cd examples/quests && go run .                        # the quest log, see ../README.md
go run . --selftest --screenshot quests.png
```
