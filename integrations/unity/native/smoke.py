#!/usr/bin/env python3
"""Loads the Unity plugin the way the C# side does (by its exported C
symbols, here through ctypes) and runs the shared HUD headless for a few
frames: create, put JS on its own thread (JsuiView.jsThread's default),
load the bundle, feed the `game` state, tick, render through jsuiu_render,
drain messages. Then one stall frame: JS kept busy for 300 ms
(jsui_debug_stall_js) while the frames go on without waiting for it, and
jsui_sync once it is done. CI runs it on the Linux build, where Unity itself
cannot run without a license.

    python3 integrations/unity/native/smoke.py <libjsui_unity.so> <bundle.js> [on|off|lockstep]
"""
import ctypes
import pathlib
import sys
import time


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int)]


def main():
    lib = ctypes.CDLL(sys.argv[1])
    bundle = pathlib.Path(sys.argv[2]).resolve().as_uri()
    P = ctypes.c_void_p
    lib.jsuiu_create.restype = P
    lib.jsuiu_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_float, ctypes.c_int, ctypes.c_int]
    lib.jsuiu_runtime.restype = P
    lib.jsuiu_runtime.argtypes = [P]
    lib.jsuiu_destroy.argtypes = [P]
    lib.jsuiu_render.argtypes = [P, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(Rect), ctypes.POINTER(P)]
    lib.jsuiu_next_message.argtypes = [P, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
    lib.jsuiu_next_log.argtypes = [P, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_char_p)]
    lib.jsui_load_bundle_url.argtypes = [P, ctypes.c_char_p]
    lib.jsui_set_state.argtypes = [P, ctypes.c_char_p, ctypes.c_char_p]
    lib.jsui_tick.argtypes = [P, ctypes.c_double]
    lib.jsui_hit_test.argtypes = [P, ctypes.c_float, ctypes.c_float]
    lib.jsui_set_js_thread.argtypes = [P, ctypes.c_int]
    lib.jsui_js_thread.argtypes = [P]
    lib.jsui_sync.argtypes = [P, ctypes.c_double]
    lib.jsui_debug_stall_js.argtypes = [P, ctypes.c_double]

    modes = {"off": 0, "on": 1, "lockstep": 2}
    mode = modes[sys.argv[3] if len(sys.argv) > 3 else "on"]
    w, h = 640, 360
    u = lib.jsuiu_create(w, h, 1.0, 0, 0)
    assert u, "jsuiu_create failed"
    rt = lib.jsuiu_runtime(u)
    lib.jsui_set_state(rt, b"game", b'{"engine":"smoke","renderer":"ctypes","fps":0,"entities":0,"paused":false,'
                       b'"passed":{"clicks":0,"wheel":0,"keys":0,"pad":0}}')
    # What JsuiView.Start does: the mode (after the game's Awake set state),
    # then the bundle.
    assert lib.jsui_set_js_thread(rt, mode) == 0, "jsui_set_js_thread failed"
    assert lib.jsui_js_thread(rt) == mode
    assert lib.jsui_load_bundle_url(rt, bundle.encode()) == 0, "jsui_load_bundle_url failed"

    messages, painted = [], 0
    rect, pixels = Rect(), P()
    clock = [0.0]

    def frame():
        nonlocal painted
        time.sleep(0.016)
        clock[0] += 16.0
        lib.jsui_tick(rt, clock[0])
        name, json = ctypes.c_char_p(), ctypes.c_char_p()
        while lib.jsuiu_next_message(u, ctypes.byref(name), ctypes.byref(json)):
            messages.append(name.value.decode())
        level, text = ctypes.c_int(), ctypes.c_char_p()
        while lib.jsuiu_next_log(u, ctypes.byref(level), ctypes.byref(text)):
            print(f"log[{level.value}]: {text.value.decode()}")
            assert level.value < 3, "the bundle logged an error"
        if lib.jsuiu_render(u, w, h, 0, ctypes.byref(rect), ctypes.byref(pixels)):
            painted += 1

    # The bundle loads on a background thread, so tick at a game's pace
    # (wall-clock 16 ms frames) until the HUD reports in, then a few more.
    deadline, settle = time.monotonic() + 10, 10
    while time.monotonic() < deadline and settle > 0:
        if "ui:ready" in messages and "ui:anchors" in messages:
            settle -= 1
        frame()

    # One stall frame: JS busy for 300 ms. On a JS thread posting it returns
    # at once and the frames go on (tick + render well under the stall);
    # off / lockstep the post itself takes the stall.
    stall = 300.0
    names = {0: "off", 1: "on", 2: "lockstep"}
    t0 = time.monotonic()
    lib.jsui_debug_stall_js(rt, stall)
    held = (time.monotonic() - t0) * 1000
    if mode == 1:
        assert held < stall / 4, f"posting the stall waited {held:.0f} ms for the JS thread"
        frames, worst = 0, 0.0
        while (time.monotonic() - t0) * 1000 < stall:
            f0 = time.monotonic()
            frame()
            worst = max(worst, (time.monotonic() - f0) * 1000 - 16)
            frames += 1
        assert frames >= 3 and worst < stall / 2, f"{frames} frames during the stall, the longest {worst:.0f} ms"
        assert lib.jsui_sync(rt, 2000) == 1, "JS did not come back from the stall"
        print(f"stall: {frames} frames during a {stall:.0f} ms JS stall, the longest tick + render {worst:.1f} ms")
    else:
        assert held >= stall * 0.9, f"the stall held the host {held:.0f} ms only"
        print(f"stall: JS thread {names[mode]}: the stall held the host {held:.0f} ms")
    for _ in range(5):
        frame()
    opaque = sum(1 for i in range(3, w * h * 4, 4 * 97) if ctypes.cast(pixels, ctypes.POINTER(ctypes.c_uint8))[i])
    lib.jsuiu_destroy(u)

    print(f"messages: {sorted(set(messages))}, painted frames: {painted}, sampled opaque pixels: {opaque}")
    assert "ui:ready" in messages and "ui:anchors" in messages, "the HUD never reported ui:ready / ui:anchors"
    assert painted > 0 and opaque > 0, "nothing was painted"
    print("unity plugin smoke: ok")


if __name__ == "__main__":
    main()
