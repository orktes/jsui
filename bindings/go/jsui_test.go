package jsui

// Round trips through libjsui without a window: shared state, the example
// UI's messages, a call into the UI and a CPU frame. Needs libjsui (see the
// package comment) and the example bundle:
//   npx jsui build bindings/ui/app.jsx -o bindings/ui/dist/bundle.js

import (
	"bytes"
	"runtime"
	"testing"
)

func newRuntime(t *testing.T) *Runtime {
	runtime.LockOSThread()
	rt, err := New(Config{Width: 640, Height: 480, AudioDisabled: true})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(rt.Close)
	return rt
}

func TestStateRoundTrip(t *testing.T) {
	rt := newRuntime(t)
	if !rt.SetState("player", `{"gold":10}`) {
		t.Fatal("first set is a change")
	}
	if rt.SetState("player", `{ "gold": 10 }`) {
		t.Fatal("an equal value is not a change")
	}
	rt.PatchState("player", `{"level":2}`)
	if v, _ := rt.GetState("player"); v != `{"gold":10,"level":2}` {
		t.Fatalf("got %s", v)
	}
	if err := rt.SetStateValue("list", []int{1, 2}); err != nil {
		t.Fatal(err)
	}
	if v, _ := rt.GetState("list"); v != `[1,2]` {
		t.Fatalf("got %s", v)
	}
	rt.RemoveState("player")
	if _, ok := rt.GetState("player"); ok {
		t.Fatal("removed")
	}
}

func TestExampleUIRuns(t *testing.T) {
	rt := newRuntime(t)
	var messages []string
	rt.OnMessage = func(name, _ string) { messages = append(messages, name) }
	rt.SetState("quests", `{"items":[{"id":1,"title":"Test quest","done":false}]}`)
	if err := rt.LoadBundleFile("../ui/dist/bundle.js"); err != nil {
		t.Fatal(err)
	}

	// No Host.handle('nope') in the UI: the call fails, inside a later tick.
	var callErr error
	answered := false
	rt.CallUI("nope", "{}", func(_ string, err error) { answered, callErr = true, err })
	for i := 0; i < 20; i++ {
		rt.Tick(float64(i) * 16)
	}
	if !answered || callErr == nil {
		t.Fatalf("CallUI: answered %v, err %v", answered, callErr)
	}
	found := false
	for _, m := range messages {
		found = found || m == "ui:anchors"
	}
	if !found {
		t.Fatalf("no ui:anchors in %v", messages)
	}

	px := make([]byte, 640*480*4)
	if err := rt.RenderRGBA(px, 640, 480); err != nil {
		t.Fatal(err)
	}
	opaque := false
	for i := 3; i < len(px); i += 4 {
		opaque = opaque || px[i] == 255
	}
	if !opaque {
		t.Fatal("the frame is empty")
	}
	png, err := rt.RenderPNG()
	if err != nil || !bytes.HasPrefix(png, []byte("\x89PNG")) {
		t.Fatalf("png: %v", err)
	}
}
