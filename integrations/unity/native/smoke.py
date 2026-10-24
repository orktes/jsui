#!/usr/bin/env python3
"""Loads the Unity plugin the way the C# side does (by its exported C
symbols, here through ctypes) and runs the shared HUD headless for a few
frames: create, load the bundle, feed the `game` state, tick, render through
jsuiu_render, drain messages. CI runs it on the Linux build, where Unity
itself cannot run without a license.

    python3 integrations/unity/native/smoke.py <libjsui_unity.so> <bundle.js>
"""
import ctypes
import pathlib
import sys


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int)]


def main():
    lib = ctypes.CDLL(sys.argv[1])
    bundle = pathlib.Path(sys.argv[2]).resolve().as_uri()
    P = ctypes.c_void_p
    lib.jsuiu_create.restype = P
    lib.jsuiu_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_float, ctypes.c_int]
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

    w, h = 640, 360
    u = lib.jsuiu_create(w, h, 1.0, 0)
    assert u, "jsuiu_create failed"
    rt = lib.jsuiu_runtime(u)
    lib.jsui_set_state(rt, b"game", b'{"engine":"smoke","renderer":"ctypes","fps":0,"entities":0,"paused":false,'
                       b'"passed":{"clicks":0,"wheel":0,"keys":0,"pad":0}}')
    assert lib.jsui_load_bundle_url(rt, bundle.encode()) == 0, "jsui_load_bundle_url failed"

    messages, painted = [], 0
    rect, pixels = Rect(), P()
    for frame in range(60):
        lib.jsui_tick(rt, frame * 16.0)
        name, json = ctypes.c_char_p(), ctypes.c_char_p()
        while lib.jsuiu_next_message(u, ctypes.byref(name), ctypes.byref(json)):
            messages.append(name.value.decode())
        level, text = ctypes.c_int(), ctypes.c_char_p()
        while lib.jsuiu_next_log(u, ctypes.byref(level), ctypes.byref(text)):
            print(f"log[{level.value}]: {text.value.decode()}")
            assert level.value < 3, "the bundle logged an error"
        if lib.jsuiu_render(u, w, h, 0, ctypes.byref(rect), ctypes.byref(pixels)):
            painted += 1
    opaque = sum(1 for i in range(3, w * h * 4, 4 * 97) if ctypes.cast(pixels, ctypes.POINTER(ctypes.c_uint8))[i])
    lib.jsuiu_destroy(u)

    print(f"messages: {sorted(set(messages))}, painted frames: {painted}, sampled opaque pixels: {opaque}")
    assert "ui:ready" in messages and "ui:anchors" in messages, "the HUD never reported ui:ready / ui:anchors"
    assert painted > 0 and opaque > 0, "nothing was painted"
    print("unity plugin smoke: ok")


if __name__ == "__main__":
    main()
