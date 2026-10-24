// Quest log: a desktop app whose UI is the React bundle in bindings/ui and
// whose data and rules live here, in Go. A GLFW window with an OpenGL
// context; Skia draws the UI on the GPU straight into the window.
//
//	npx jsui build bindings/ui/app.jsx -o bindings/ui/dist/bundle.js
//	cd bindings/go/examples/quests && go run .        # [bundle.js | --dev URL]
//	go run . --selftest --screenshot quests.png
//
// What crosses the boundary (see bindings/ui/app.jsx):
//
//	Go -> UI  shared state host, quests, player, clock; message toast
//	UI -> Go  calls quests.add / quests.toggle / quests.remove; messages
//	          ui:anchors (for --selftest) and app:quit
package main

/*
typedef const unsigned char* (*get_string_fn)(unsigned int);
typedef void (*read_pixels_fn)(int, int, int, int, unsigned int, unsigned int, void*);
static const char* gl_get_string(void* f, unsigned int name) { return (const char*)((get_string_fn)f)(name); }
static void gl_read_rgba(void* f, int w, int h, void* out) { ((read_pixels_fn)f)(0, 0, w, h, 0x1908, 0x1401, out); }
*/
import "C"

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/png"
	"log"
	"os"
	"path/filepath"
	goruntime "runtime"
	"strings"
	"time"
	"unsafe"

	"github.com/go-gl/glfw/v3.3/glfw"
	"github.com/orktes/jsui/bindings/go"
)

func init() { goruntime.LockOSThread() } // GLFW, GL and the runtime live on the main thread

type Quest struct {
	ID    uint64 `json:"id"`
	Title string `json:"title"`
	Done  bool   `json:"done"`
}

// App is everything the program knows. The UI only sees it through shared state.
type App struct {
	Quests  []Quest
	nextID  uint64
	XP      int
	Level   int
	anchors map[string]json.RawMessage
	quit    bool
}

func NewApp() *App {
	a := &App{nextID: 1, Level: 1}
	for _, t := range []string{"Slay the ember drake", "Map the sunken library", "Forge a starsteel blade"} {
		a.add(t)
	}
	return a
}

func (a *App) add(title string) uint64 {
	id := a.nextID
	a.nextID++
	a.Quests = append(a.Quests, Quest{ID: id, Title: title})
	return id
}

func (a *App) nextLevelXP() int { return 100 * a.Level }

// publish pushes the app's data to the UI. Unchanged values cost nothing.
func (a *App) publish(rt *jsui.Runtime) {
	rt.SetStateValue("quests", map[string]any{"items": a.Quests})
	rt.SetStateValue("player", map[string]int{"level": a.Level, "xp": a.XP, "next": a.nextLevelXP()})
}

// handle answers Host.call(name, args) from the UI: the app's rules.
func (a *App) handle(rt *jsui.Runtime, c jsui.Call) (any, error) {
	var args struct {
		ID    uint64 `json:"id"`
		Title string `json:"title"`
	}
	if err := c.Decode(&args); err != nil {
		return nil, err
	}
	var result any
	switch c.Name {
	case "quests.add":
		title := strings.TrimSpace(args.Title)
		if title == "" {
			return nil, errors.New("empty title")
		}
		result = map[string]uint64{"id": a.add(title)}
	case "quests.toggle":
		i := a.find(args.ID)
		if i < 0 {
			return nil, errors.New("no such quest")
		}
		q := &a.Quests[i]
		q.Done = !q.Done
		if q.Done {
			a.XP += 50
			text, tone := fmt.Sprintf("Quest complete: %s (+50 XP)", q.Title), "quest"
			for a.XP >= a.nextLevelXP() {
				a.XP -= a.nextLevelXP()
				a.Level++
				text, tone = fmt.Sprintf("Level up! You are now level %d", a.Level), "level"
			}
			rt.PostMessageValue("toast", map[string]string{"text": text, "tone": tone})
		}
		result = map[string]bool{"done": q.Done}
	case "quests.remove":
		if i := a.find(args.ID); i >= 0 {
			a.Quests = append(a.Quests[:i], a.Quests[i+1:]...)
		}
		result = struct{}{}
	default:
		return nil, fmt.Errorf("unknown call %s", c.Name)
	}
	a.publish(rt)
	return result, nil
}

func (a *App) find(id uint64) int {
	for i, q := range a.Quests {
		if q.ID == id {
			return i
		}
	}
	return -1
}

func main() {
	_, self, _, _ := goruntime.Caller(0)
	defaultBundle := filepath.Join(filepath.Dir(self), "../../../ui/dist/bundle.js")
	dev := flag.Bool("dev", false, "the bundle is a `jsui dev` URL: hot-update on change")
	selftest := flag.Bool("selftest", false, "scripted check of the UI <-> Go round trip, then exit")
	screenshot := flag.String("screenshot", "", "save the last frame (read back from the GPU) as PNG")
	frames := flag.Int("frames", 0, "exit after N frames")
	width := flag.Int("width", 900, "window width (logical px)")
	height := flag.Int("height", 760, "window height (logical px)")
	flag.Parse()
	bundle := defaultBundle
	if flag.NArg() > 0 {
		bundle = flag.Arg(0)
	}

	// --- window and GL context
	if err := glfw.Init(); err != nil {
		log.Fatal(err)
	}
	defer glfw.Terminate()
	glfw.WindowHint(glfw.ContextVersionMajor, 3)
	glfw.WindowHint(glfw.ContextVersionMinor, 3)
	glfw.WindowHint(glfw.OpenGLProfile, glfw.OpenGLCoreProfile)
	glfw.WindowHint(glfw.OpenGLForwardCompatible, glfw.True)
	glfw.WindowHint(glfw.ScaleToMonitor, glfw.True)
	win, err := glfw.CreateWindow(*width, *height, "JSUI · Go", nil, nil)
	if err != nil {
		log.Fatal("could not create a window with an OpenGL 3.3 context: ", err)
	}
	win.MakeContextCurrent()
	if *selftest {
		glfw.SwapInterval(0)
	} else {
		glfw.SwapInterval(1)
	}

	gpu, err := jsui.NewGPUContextGL()
	if err != nil {
		log.Fatal(err)
	}
	gl := loadGL()
	gpuName := gl.renderer()
	fmt.Println("GPU: Skia Ganesh GL on", gpuName)

	// --- runtime
	w, h := win.GetSize()
	pixelRatio := func() float32 {
		w, _ := win.GetSize()
		fw, _ := win.GetFramebufferSize()
		return float32(fw) / float32(max(w, 1))
	}
	rt, err := jsui.New(jsui.Config{Width: w, Height: h, PixelRatio: pixelRatio(), DevMode: *dev})
	if err != nil {
		log.Fatal(err)
	}
	defer rt.Close()

	app := NewApp()
	rt.OnCall = func(c jsui.Call) {
		if v, err := app.handle(rt, c); err != nil {
			rt.RejectCall(c.ID, err.Error())
		} else {
			rt.ResolveCallValue(c.ID, v)
		}
	}
	rt.OnMessage = func(name, payload string) {
		switch name {
		case "ui:anchors":
			app.anchors = nil
			json.Unmarshal([]byte(payload), &app.anchors)
		case "app:quit":
			app.quit = true
		}
	}
	rt.SetStateValue("host", map[string]string{
		"language": "Go " + strings.TrimPrefix(goruntime.Version(), "go"),
		"renderer": "Skia Ganesh · OpenGL",
		"gpu":      gpuName,
		"os":       goruntime.GOOS + " " + goruntime.GOARCH,
	})
	app.publish(rt)

	if *dev || strings.HasPrefix(bundle, "http") {
		err = rt.LoadBundleURL(bundle)
	} else {
		err = rt.LoadBundleFile(bundle)
	}
	if err != nil {
		log.Fatalf("could not load %s: %v\n(build it: npx jsui build bindings/ui/app.jsx -o bindings/ui/dist/bundle.js)", bundle, err)
	}

	// --- input: every event goes to the UI first
	var mx, my float32
	dirty := true
	resize := func() {
		w, h := win.GetSize()
		rt.SetViewport(w, h, pixelRatio())
		dirty = true
	}
	win.SetSizeCallback(func(*glfw.Window, int, int) { resize() })
	win.SetFramebufferSizeCallback(func(*glfw.Window, int, int) { resize() })
	win.SetContentScaleCallback(func(*glfw.Window, float32, float32) { resize() })
	win.SetRefreshCallback(func(*glfw.Window) { dirty = true })
	win.SetCursorPosCallback(func(_ *glfw.Window, x, y float64) {
		mx, my = float32(x), float32(y)
		rt.SendMouseMove(mx, my)
	})
	win.SetCursorEnterCallback(func(_ *glfw.Window, entered bool) {
		if !entered {
			rt.SendMouseLeave()
		}
	})
	win.SetMouseButtonCallback(func(_ *glfw.Window, b glfw.MouseButton, action glfw.Action, m glfw.ModifierKey) {
		button := map[glfw.MouseButton]jsui.MouseButton{glfw.MouseButtonRight: jsui.MouseRight, glfw.MouseButtonMiddle: jsui.MouseMiddle}[b]
		consumed := rt.SendMouseButton(mx, my, button, action == glfw.Press, mods(m))
		// Input the UI did not consume is the program's (a game would orbit its camera here).
		if !consumed && action == glfw.Press {
			fmt.Printf("click at %.0f,%.0f passed through to the app\n", mx, my)
		}
	})
	win.SetScrollCallback(func(_ *glfw.Window, dx, dy float64) {
		rt.SendMouseWheel(mx, my, float32(-dx*40), float32(-dy*40))
	})
	win.SetKeyCallback(func(_ *glfw.Window, k glfw.Key, _ int, action glfw.Action, m glfw.ModifierKey) {
		key, code := domKey(k)
		rt.SendKey(key, code, action != glfw.Release, action == glfw.Repeat, mods(m))
	})
	win.SetCharCallback(func(_ *glfw.Window, r rune) { rt.SendText(string(r)) })

	cursors := map[string]*glfw.Cursor{
		"pointer":   glfw.CreateStandardCursor(glfw.HandCursor),
		"text":      glfw.CreateStandardCursor(glfw.IBeamCursor),
		"crosshair": glfw.CreateStandardCursor(glfw.CrosshairCursor),
	}
	cursor := "default"

	// --- frame loop
	clock := ""
	test := &selfTest{}
	for frame := 1; !win.ShouldClose() && !app.quit; frame++ {
		// Sleep while nothing moves; the UI's animations ask for frames.
		if *selftest || *frames > 0 || dirty || rt.WantsFrame() {
			glfw.PollEvents()
		} else {
			glfw.WaitEventsTimeout(0.25)
		}

		if now := time.Now().Format("15:04:05"); now != clock {
			clock = now
			rt.SetStateValue("clock", clock)
		}

		rt.Tick(jsui.NowMs())

		if want := rt.Cursor(); want != cursor {
			win.SetCursor(cursors[want]) // nil: the arrow
			cursor = want
		}

		if rt.NeedsRedraw() || dirty {
			fw, fh := win.GetFramebufferSize()
			rt.RenderGL(gpu, 0, fw, fh, 0)
			win.SwapBuffers()
			dirty = false
		}

		if *selftest {
			done, err := test.step(rt, app, frame)
			if err != nil {
				fmt.Fprintln(os.Stderr, "selftest: FAILED:", err)
				os.Exit(1)
			}
			if done {
				fmt.Println("selftest: ok")
				break
			}
		} else if *frames > 0 && frame >= *frames {
			break
		}
	}

	if *screenshot != "" {
		// Settle animations, then read back what the GPU drew.
		for i := 0; i < 30; i++ {
			rt.Tick(jsui.NowMs() + 1000)
		}
		fw, fh := win.GetFramebufferSize()
		rt.RenderGL(gpu, 0, fw, fh, 0)
		if err := writePNG(*screenshot, gl.readPixels(fw, fh)); err != nil {
			log.Fatal(err)
		}
		fmt.Printf("wrote %s (%dx%d)\n", *screenshot, fw, fh)
	}
	gpu.Close() // GL context still current
}

func mods(m glfw.ModifierKey) int {
	out := 0
	if m&glfw.ModShift != 0 {
		out |= jsui.ModShift
	}
	if m&glfw.ModControl != 0 {
		out |= jsui.ModCtrl
	}
	if m&glfw.ModAlt != 0 {
		out |= jsui.ModAlt
	}
	if m&glfw.ModSuper != 0 {
		out |= jsui.ModMeta
	}
	return out
}

var namedKeys = map[glfw.Key][2]string{
	glfw.KeyEnter: {"Enter", "Enter"}, glfw.KeyKPEnter: {"Enter", "NumpadEnter"}, glfw.KeyEscape: {"Escape", "Escape"},
	glfw.KeyTab: {"Tab", "Tab"}, glfw.KeyBackspace: {"Backspace", "Backspace"}, glfw.KeyDelete: {"Delete", "Delete"},
	glfw.KeyLeft: {"ArrowLeft", "ArrowLeft"}, glfw.KeyRight: {"ArrowRight", "ArrowRight"},
	glfw.KeyUp: {"ArrowUp", "ArrowUp"}, glfw.KeyDown: {"ArrowDown", "ArrowDown"},
	glfw.KeyHome: {"Home", "Home"}, glfw.KeyEnd: {"End", "End"},
	glfw.KeyPageUp: {"PageUp", "PageUp"}, glfw.KeyPageDown: {"PageDown", "PageDown"}, glfw.KeySpace: {" ", "Space"},
	glfw.KeyLeftShift: {"Shift", "ShiftLeft"}, glfw.KeyRightShift: {"Shift", "ShiftRight"},
	glfw.KeyLeftControl: {"Control", "ControlLeft"}, glfw.KeyRightControl: {"Control", "ControlRight"},
	glfw.KeyLeftAlt: {"Alt", "AltLeft"}, glfw.KeyRightAlt: {"Alt", "AltRight"},
	glfw.KeyLeftSuper: {"Meta", "MetaLeft"}, glfw.KeyRightSuper: {"Meta", "MetaRight"},
}

// domKey maps a GLFW key to DOM key and code (text itself comes from the char callback).
func domKey(k glfw.Key) (string, string) {
	if n, ok := namedKeys[k]; ok {
		return n[0], n[1]
	}
	if k >= glfw.KeyA && k <= glfw.KeyZ {
		c := string(rune('a' + k - glfw.KeyA))
		return c, "Key" + strings.ToUpper(c)
	}
	if k >= glfw.Key0 && k <= glfw.Key9 {
		c := string(rune('0' + k - glfw.Key0))
		return c, "Digit" + c
	}
	return "", ""
}

// selfTest is a scripted check of the whole round trip, driven by where the
// UI says its controls are (ui:anchors): a click on empty space passes
// through, Add creates a quest in Go, ticking the first quest gives 50 XP and
// the UI shows both.
type selfTest struct {
	stage int
	count int
}

type rect struct{ X, Y, Width, Height float32 }

func (s *selfTest) step(rt *jsui.Runtime, app *App, frame int) (bool, error) {
	if frame > 900 {
		return false, fmt.Errorf("timed out in stage %d (anchors: %v)", s.stage, app.anchors)
	}
	center := func(name string) (float32, float32, bool) {
		var r rect
		if raw, ok := app.anchors[name]; !ok || json.Unmarshal(raw, &r) != nil {
			return 0, 0, false
		}
		return r.X + r.Width/2, r.Y + r.Height/2, true
	}
	click := func(x, y float32) bool {
		rt.SendMouseMove(x, y)
		down := rt.SendMouseButton(x, y, jsui.MouseLeft, true, 0)
		up := rt.SendMouseButton(x, y, jsui.MouseLeft, false, 0)
		return down && up
	}
	num := func(name string) int {
		var n int
		json.Unmarshal(app.anchors[name], &n)
		return n
	}
	count, done := num("count"), num("done")
	switch {
	case s.stage == 0 && count > 0:
		ax, ay, ok := center("add")
		ex, ey, _ := center("empty")
		if !ok {
			return false, nil
		}
		if rt.HitTest(ex, ey) != 0 || click(ex, ey) {
			return false, errors.New("a click on empty space was consumed")
		}
		if !click(ax, ay) {
			return false, errors.New("the Add button did not consume its click")
		}
		s.count, s.stage = count, 1
	case s.stage == 1 && count == s.count+1 && len(app.Quests) == count:
		tx, ty, ok := center("toggle")
		if !ok || done != 0 || !click(tx, ty) {
			return false, errors.New("could not tick the first quest")
		}
		s.stage = 2
	case s.stage == 2 && done == 1:
		if app.XP != 50 || !app.Quests[0].Done {
			return false, fmt.Errorf("expected 50 XP, have %d", app.XP)
		}
		if p, _ := rt.GetState("player"); !strings.Contains(p, `"xp":50`) {
			return false, fmt.Errorf("player state not published: %s", p)
		}
		return true, nil
	}
	return false, nil
}

// gl is the two GL entry points the example calls itself (Skia loads its own).
type gl struct{ getString, readPixelsFn unsafe.Pointer }

func loadGL() gl {
	return gl{glfw.GetProcAddress("glGetString"), glfw.GetProcAddress("glReadPixels")}
}

func (g gl) renderer() string {
	const glRenderer = 0x1F01
	if p := C.gl_get_string(g.getString, glRenderer); p != nil {
		return C.GoString(p)
	}
	return "unknown GPU"
}

// readPixels returns the framebuffer, top row first.
func (g gl) readPixels(w, h int) *image.NRGBA {
	px := make([]byte, w*h*4)
	C.gl_read_rgba(g.readPixelsFn, C.int(w), C.int(h), unsafe.Pointer(&px[0]))
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		copy(img.Pix[y*img.Stride:], px[(h-1-y)*w*4:(h-y)*w*4])
	}
	for i := 3; i < len(img.Pix); i += 4 {
		img.Pix[i] = 255
	}
	return img
}

func writePNG(path string, img image.Image) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}
