// Package jsui embeds JSUI in Go programs: UIs written in React (React
// Native components), laid out with Yoga and drawn by Skia, through libjsui's
// C API.
//
//	rt, err := jsui.New(jsui.Config{Width: 1280, Height: 720, PixelRatio: 1})
//	rt.OnMessage = func(name, payload string) { log.Println("UI says", name, payload) }
//	rt.OnCall = func(c jsui.Call) {
//		if c.Name == "add" {
//			rt.ResolveCallValue(c.ID, map[string]any{"ok": true})
//		} else {
//			rt.RejectCall(c.ID, "unknown call")
//		}
//	}
//	rt.SetStateValue("player", Player{Name: "Ada", Level: 3})
//	rt.LoadBundleFile("ui/dist/bundle.js")
//	for !window.ShouldClose() {
//		rt.Tick(jsui.NowMs())
//		if rt.NeedsRedraw() {
//			rt.RenderGL(gpu, 0, fbWidth, fbHeight, 0) // GL context current
//			window.SwapBuffers()
//		}
//	}
//
// The runtime is single threaded: call it (and create it) from one
// goroutine, locked to its OS thread (runtime.LockOSThread) like any GL or
// windowing code. Callbacks run on that goroutine while the runtime is
// inside a call, usually Tick. Values cross the boundary as JSON text; the
// *Value helpers marshal with encoding/json.
//
// Linking: libjsui (libjsui.so / libjsui.dylib) is looked up in the
// repository's build/ directory (cmake -B build -DJSUI_BUILD_SHARED=ON) with
// that directory in the rpath. For a release archive or another location set
// CGO_LDFLAGS="-L/path/to/lib -Wl,-rpath,/path/to/lib".
package jsui

/*
#cgo CFLAGS: -I${SRCDIR}/include
#cgo LDFLAGS: -L${SRCDIR}/../../build -Wl,-rpath,${SRCDIR}/../../build -ljsui
#include <stdlib.h>
#include "shim.h"
*/
import "C"

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime/cgo"
	"time"
	"unsafe"
)

// Config holds the options of New.
type Config struct {
	Width, Height int     // viewport in logical px (default 1280 x 720)
	PixelRatio    float32 // device px per logical px (default 1)
	BaseURL       string  // resolves relative URLs; default: the bundle's URL
	DevMode       bool    // poll the `jsui dev` server and hot-update on change
	AudioDisabled bool    // no sound at all
}

// LogLevel is the severity passed to Runtime.OnLog.
type LogLevel int

const (
	LogDebug LogLevel = iota
	LogInfo
	LogWarn
	LogError
)

// Call is a Host.call(name, args) from the UI. Answer it exactly once, now or
// on a later frame, with ResolveCall / ResolveCallValue or RejectCall.
type Call struct {
	ID   uint64
	Name string
	Args string // JSON
}

// Decode unmarshals the call's arguments into v.
func (c Call) Decode(v any) error { return json.Unmarshal([]byte(c.Args), v) }

// MouseButton for SendMouseButton.
type MouseButton int

const (
	MouseLeft   MouseButton = C.JSUI_MOUSE_LEFT
	MouseMiddle MouseButton = C.JSUI_MOUSE_MIDDLE
	MouseRight  MouseButton = C.JSUI_MOUSE_RIGHT
)

// Modifier bits for input events.
const (
	ModShift = C.JSUI_MOD_SHIFT
	ModCtrl  = C.JSUI_MOD_CTRL
	ModAlt   = C.JSUI_MOD_ALT
	ModMeta  = C.JSUI_MOD_META
)

// Flags for RenderGL.
const (
	GLTopLeftOrigin = C.JSUI_GL_TOP_LEFT_ORIGIN // row 0 is the top (D3D-style texture UVs)
	GLNoClear       = C.JSUI_GL_NO_CLEAR        // draw over the target (a HUD over the game)
	GLNoRetain      = C.JSUI_GL_NO_RETAIN       // repaint the whole frame every render
)

// HitTest bits.
const (
	HitPointer = C.JSUI_HIT_POINTER
	HitScroll  = C.JSUI_HIT_SCROLL
)

// Runtime is one bundle, its JS context, its shared state and its main
// surface. Set the On* handlers any time; nil ones are skipped (calls
// without OnCall are rejected, logs without OnLog go to stderr).
type Runtime struct {
	rt     *C.JsuiRuntime
	handle cgo.Handle

	OnMessage    func(name, payload string)       // Host.postMessage(name, payload); payload is JSON
	OnState      func(key string, value *string)  // the UI changed shared state (nil: removed)
	OnCall       func(call Call)                  // Host.call(name, args)
	OnCallCancel func(id uint64)                  // a call ended unanswered (JS timeout, reload)
	OnLog        func(level LogLevel, msg string) // runtime and console.* output
}

// New creates a runtime. Load a bundle next, then Tick it every frame.
func New(cfg Config) (*Runtime, error) {
	r := &Runtime{}
	r.handle = cgo.NewHandle(r)
	var c C.JsuiConfig
	C.jsui_config_init(&c)
	if cfg.Width > 0 {
		c.width = C.int(cfg.Width)
	}
	if cfg.Height > 0 {
		c.height = C.int(cfg.Height)
	}
	if cfg.PixelRatio > 0 {
		c.pixel_ratio = C.float(cfg.PixelRatio)
	}
	if cfg.BaseURL != "" {
		s := C.CString(cfg.BaseURL)
		defer C.free(unsafe.Pointer(s))
		c.base_url = s
	}
	if cfg.DevMode {
		c.dev_mode = 1
	}
	if cfg.AudioDisabled {
		c.audio_disabled = 1
	}
	C.gojsui_set_callbacks(&c, C.uintptr_t(r.handle))
	r.rt = C.jsui_create(&c)
	if r.rt == nil {
		r.handle.Delete()
		return nil, errors.New("jsui: jsui_create failed")
	}
	return r, nil
}

// Close destroys the runtime. Calls still waiting in CallUI get their error.
func (r *Runtime) Close() {
	if r.rt == nil {
		return
	}
	C.jsui_destroy(r.rt)
	r.rt = nil
	r.handle.Delete()
}

// Ptr is the raw JsuiRuntime*, for C API calls this package does not wrap.
func (r *Runtime) Ptr() unsafe.Pointer { return unsafe.Pointer(r.rt) }

// cstr converts for one call; free with the returned func.
func cstr(s string) (*C.char, func()) {
	c := C.CString(s)
	return c, func() { C.free(unsafe.Pointer(c)) }
}

func b(v C.int) bool { return v != 0 }

// ---------------------------------------------------------------- content

// LoadBundleSource evaluates a bundle (npx jsui build app.jsx); name shows in stack traces.
func (r *Runtime) LoadBundleSource(source, name string) error {
	src := C.CBytes([]byte(source))
	defer C.free(src)
	n, free := cstr(name)
	defer free()
	if C.jsui_load_bundle_source(r.rt, (*C.char)(src), C.size_t(len(source)), n) != 0 {
		return r.errorOr("bundle failed to load")
	}
	return nil
}

// LoadBundleFile reads and evaluates a bundle file; relative URLs resolve next to it.
func (r *Runtime) LoadBundleFile(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		abs = path
	}
	return r.LoadBundleSource(string(data), "file://"+filepath.ToSlash(abs))
}

// LoadBundleURL loads a bundle through the loader (a path, file:// or http://, e.g. `jsui dev`).
func (r *Runtime) LoadBundleURL(url string) error {
	u, free := cstr(url)
	defer free()
	if C.jsui_load_bundle_url(r.rt, u) != 0 {
		return r.errorOr("bundle failed to load")
	}
	return nil
}

// Reload tears down the JS context and loads the last bundle again.
func (r *Runtime) Reload() { C.jsui_reload(r.rt) }

// LastError is the last uncaught JS error (message and stack), or "".
func (r *Runtime) LastError() string { return goString(C.jsui_last_error(r.rt)) }

func (r *Runtime) errorOr(fallback string) error {
	if e := r.LastError(); e != "" {
		return errors.New(e)
	}
	return errors.New(fallback)
}

func goString(p *C.char) string {
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// ------------------------------------------------------ talking to the UI

// SetState sets shared state key to JSON text (useHostState(key) in the UI).
// It reports whether the value changed; json must be valid JSON.
func (r *Runtime) SetState(key, json string) bool {
	k, fk := cstr(key)
	defer fk()
	j, fj := cstr(json)
	defer fj()
	return C.jsui_set_state(r.rt, k, j) == 1
}

// SetStateValue marshals v with encoding/json into shared state key.
func (r *Runtime) SetStateValue(key string, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	r.SetState(key, string(data))
	return nil
}

// PatchState applies an RFC 7386 JSON merge patch to shared state key.
func (r *Runtime) PatchState(key, mergePatch string) bool {
	k, fk := cstr(key)
	defer fk()
	j, fj := cstr(mergePatch)
	defer fj()
	return C.jsui_patch_state(r.rt, k, j) == 1
}

// RemoveState removes shared state key.
func (r *Runtime) RemoveState(key string) bool {
	k, fk := cstr(key)
	defer fk()
	return C.jsui_set_state(r.rt, k, nil) == 1
}

// GetState returns the JSON text of shared state key.
func (r *Runtime) GetState(key string) (string, bool) {
	k, fk := cstr(key)
	defer fk()
	p := C.jsui_get_state(r.rt, k)
	if p == nil {
		return "", false
	}
	return C.GoString(p), true
}

// PostMessage sends name with a JSON payload to Host.onMessage / useHostMessage.
func (r *Runtime) PostMessage(name, json string) {
	n, fn := cstr(name)
	defer fn()
	j, fj := cstr(json)
	defer fj()
	C.jsui_post_message(r.rt, n, j)
}

// PostMessageValue marshals v with encoding/json and posts it.
func (r *Runtime) PostMessageValue(name string, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	r.PostMessage(name, string(data))
	return nil
}

// ResolveCall answers a Call: its promise resolves with the parsed JSON. It
// reports false for an unknown or settled call or invalid JSON.
func (r *Runtime) ResolveCall(id uint64, json string) bool {
	j, fj := cstr(json)
	defer fj()
	return C.jsui_resolve_call(r.rt, C.uint64_t(id), j) == 0
}

// ResolveCallValue marshals v with encoding/json and resolves the call with it.
func (r *Runtime) ResolveCallValue(id uint64, v any) bool {
	data, err := json.Marshal(v)
	if err != nil {
		return r.RejectCall(id, err.Error())
	}
	return r.ResolveCall(id, string(data))
}

// RejectCall rejects a Call with Error(message).
func (r *Runtime) RejectCall(id uint64, message string) bool {
	m, fm := cstr(message)
	defer fm()
	return C.jsui_reject_call(r.rt, C.uint64_t(id), m) == 0
}

// CallUI runs the UI's Host.handle(name, fn) handler. done gets its result as
// JSON text, or an error, inside a later Tick (or from Close). It returns the
// call id.
func (r *Runtime) CallUI(name, argsJSON string, done func(result string, err error)) uint64 {
	n, fn := cstr(name)
	defer fn()
	a, fa := cstr(argsJSON)
	defer fa()
	h := cgo.NewHandle(done)
	return uint64(C.gojsui_call_ui(r.rt, n, a, C.uintptr_t(h)))
}

// SetBufferF32 sets frame-data buffer key (a Float32Array in the UI:
// useHostBufferValue). It reports whether the contents changed.
func (r *Runtime) SetBufferF32(key string, data []float32) bool {
	k, fk := cstr(key)
	defer fk()
	var p unsafe.Pointer
	if len(data) > 0 {
		p = unsafe.Pointer(&data[0])
	}
	return C.jsui_set_buffer(r.rt, k, C.JSUI_BUFFER_F32, p, C.size_t(len(data))) == 1
}

// RemoveBuffer removes frame-data buffer key.
func (r *Runtime) RemoveBuffer(key string) bool {
	k, fk := cstr(key)
	defer fk()
	return C.jsui_remove_buffer(r.rt, k) == 1
}

// SetTexturePixels shows width x height RGBA8 pixels (sRGB, unpremultiplied)
// as host texture key (<HostTexture source="key">).
func (r *Runtime) SetTexturePixels(key string, rgba []byte, width, height int) error {
	if len(rgba) < width*height*4 || width <= 0 || height <= 0 {
		return fmt.Errorf("jsui: %d bytes for a %dx%d texture", len(rgba), width, height)
	}
	k, fk := cstr(key)
	defer fk()
	C.jsui_set_texture_pixels(r.rt, k, unsafe.Pointer(&rgba[0]), C.int(width), C.int(height), 0, 0)
	return nil
}

// RegisterFont registers a font (TTF, OTF, TTC, WOFF, WOFF2) under family ("": its own name).
func (r *Runtime) RegisterFont(family string, data []byte) error {
	if len(data) == 0 {
		return errors.New("jsui: empty font")
	}
	f, ff := cstr(family)
	defer ff()
	if C.jsui_register_font(r.rt, f, unsafe.Pointer(&data[0]), C.size_t(len(data)), 0, -1) != 0 {
		return errors.New("jsui: not a font")
	}
	return nil
}

// ------------------------------------------------------------- frame loop

// SetViewport sets the viewport in logical px and the device pixel ratio.
func (r *Runtime) SetViewport(width, height int, pixelRatio float32) {
	C.jsui_set_viewport(r.rt, C.int(width), C.int(height), C.float(pixelRatio))
}

// Tick advances to nowMs (monotonic, e.g. NowMs): timers, loads, animation
// frames, layout. Handlers run in here.
func (r *Runtime) Tick(nowMs float64) { C.jsui_tick(r.rt, C.double(nowMs)) }

// NeedsRedraw reports whether the last tick changed what the UI would paint.
func (r *Runtime) NeedsRedraw() bool { return b(C.jsui_needs_redraw(r.rt)) }

// WantsFrame reports whether something is moving: keep ticking at display
// rate even without input.
func (r *Runtime) WantsFrame() bool { return b(C.jsui_wants_frame(r.rt)) }

// PendingLoads is the number of resource loads still in flight.
func (r *Runtime) PendingLoads() int { return int(C.jsui_pending_loads(r.rt)) }

// RenderGL paints the frame with Skia on the GPU into GL framebuffer fbo (0:
// the window's) of width x height device px. gpu's GL context must be current.
func (r *Runtime) RenderGL(gpu *GPUContext, fbo uint32, width, height, flags int) bool {
	return C.jsui_render_gl(r.rt, gpu.ptr, C.uint(fbo), C.int(width), C.int(height), C.int(flags)) == 0
}

// RenderRGBA paints the frame on the CPU into premultiplied RGBA8 pixels
// (width * height * 4 bytes).
func (r *Runtime) RenderRGBA(pixels []byte, width, height int) error {
	if len(pixels) < width*height*4 || width <= 0 || height <= 0 {
		return fmt.Errorf("jsui: %d bytes for a %dx%d frame", len(pixels), width, height)
	}
	C.jsui_render_rgba(r.rt, unsafe.Pointer(&pixels[0]), C.int(width), C.int(height), C.size_t(width*4))
	return nil
}

// RenderPNG returns the frame as a PNG (CPU raster).
func (r *Runtime) RenderPNG() ([]byte, error) {
	var data unsafe.Pointer
	var size C.size_t
	if C.jsui_render_png(r.rt, &data, &size) != 0 || data == nil {
		return nil, errors.New("jsui: render_png failed")
	}
	defer C.jsui_free(data)
	return C.GoBytes(data, C.int(size)), nil
}

// ------------------------------------------------------------------ input
// Coordinates are logical px from the viewport's top-left. Each Send*
// reports whether the UI consumed the event; when it did not, it is yours.

func (r *Runtime) SendMouseMove(x, y float32) bool {
	return b(C.jsui_send_mouse_move(r.rt, C.float(x), C.float(y)))
}

func (r *Runtime) SendMouseButton(x, y float32, button MouseButton, down bool, mods int) bool {
	return b(C.jsui_send_mouse_button(r.rt, C.float(x), C.float(y), C.JsuiMouseButton(button), cbool(down), C.int(mods)))
}

func (r *Runtime) SendMouseWheel(x, y, dx, dy float32) bool {
	return b(C.jsui_send_mouse_wheel(r.rt, C.float(x), C.float(y), C.float(dx), C.float(dy)))
}

func (r *Runtime) SendMouseLeave() bool { return b(C.jsui_send_mouse_leave(r.rt)) }

// SendKey sends a key in DOM terms: key "a" / "Enter" / "ArrowLeft", code
// "KeyA" / "Enter" ("" for unknown).
func (r *Runtime) SendKey(key, code string, down, repeat bool, mods int) bool {
	var k, c *C.char
	if key != "" {
		k = C.CString(key)
		defer C.free(unsafe.Pointer(k))
	}
	if code != "" {
		c = C.CString(code)
		defer C.free(unsafe.Pointer(c))
	}
	return b(C.jsui_send_key(r.rt, k, c, cbool(down), cbool(repeat), C.int(mods)))
}

// SendText sends typed text (after the key event).
func (r *Runtime) SendText(text string) bool {
	t, ft := cstr(text)
	defer ft()
	return b(C.jsui_send_text(r.rt, t))
}

// SendGamepadButton sends a button in the W3C standard mapping (0 A, 1 B, 12-15 D-pad, ...).
func (r *Runtime) SendGamepadButton(pad, button int, down bool, value float32) bool {
	return b(C.jsui_send_gamepad_button(r.rt, C.int(pad), C.int(button), cbool(down), C.float(value)))
}

func (r *Runtime) SendGamepadAxis(pad, axis int, value float32) bool {
	return b(C.jsui_send_gamepad_axis(r.rt, C.int(pad), C.int(axis), C.float(value)))
}

// HitTest reports what the UI would capture at a point (Hit* bits); 0 passes through.
func (r *Runtime) HitTest(x, y float32) int {
	return int(C.jsui_hit_test(r.rt, C.float(x), C.float(y)))
}

// Cursor is the CSS cursor the UI wants ("default", "pointer", "text", ...).
func (r *Runtime) Cursor() string {
	if s := goString(C.jsui_get_cursor(r.rt)); s != "" {
		return s
	}
	return "default"
}

func cbool(v bool) C.int {
	if v {
		return 1
	}
	return 0
}

// ------------------------------------------------------------------- GPU

// GPUContext is Skia on the program's OpenGL context. Create it, use it and
// Close it with that GL context current.
type GPUContext struct{ ptr *C.JsuiGpuContext }

// NewGPUContextGL makes a Skia Ganesh context on the current GL context, with
// GL functions from the platform's loader (GLX / EGL, OpenGL.framework, WGL).
func NewGPUContextGL() (*GPUContext, error) {
	p := C.jsui_gpu_create_gl(nil, nil)
	if p == nil {
		return nil, errors.New("jsui: no usable GL context (or a library built without GPU)")
	}
	return &GPUContext{ptr: p}, nil
}

// ContextLost drops GPU objects without calling GL (the context is gone).
func (g *GPUContext) ContextLost() { C.jsui_gpu_context_lost(g.ptr) }

// Close frees the context; the GL context must be current.
func (g *GPUContext) Close() {
	if g.ptr != nil {
		C.jsui_gpu_destroy(g.ptr)
		g.ptr = nil
	}
}

var start = time.Now()

// NowMs is milliseconds on a monotonic clock, for Tick.
func NowMs() float64 { return float64(time.Since(start).Nanoseconds()) / 1e6 }
