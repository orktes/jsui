# JSUI for Rust

```rust
let rt = jsui::Runtime::new(jsui::Config { width: 900, height: 760, pixel_ratio: 2.0, ..Default::default() })?;
rt.on_call(|cx, call| match call.name {
    "quests.add" => { cx.resolve_call(call.id, r#"{"id":4}"#); }
    _ => { cx.reject_call(call.id, "unknown call"); }
});
rt.on_message(|_, name, payload| println!("{name}: {payload}"));
rt.set_state("player", r#"{"level":3,"xp":120}"#);       // useHostState('player')
rt.load_bundle_file("bindings/ui/dist/bundle.js")?;

let gpu = jsui::GpuContext::gl().unwrap();                // the window's GL context is current
loop {
    // events: rt.send_mouse_move / send_mouse_button / send_key / send_text ...
    rt.tick(jsui::now_ms());
    if rt.needs_redraw() {
        rt.render_gl(&gpu, 0, fb_width, fb_height, 0);    // Skia on the GPU, into the window
        // swap buffers
    }
}
```

* `jsui::sys`: the C API declared by hand (`#[repr(C)]` structs, `extern "C"`).
* `jsui::Runtime` owns a runtime and dereferences to `jsui::Context`, which has
  every method and is what callbacks get (answer calls, set state, post
  messages from inside a callback). `Runtime` is neither `Send` nor `Sync`.
* Callbacks are closures (`on_message`, `on_call`, `on_call_cancel`,
  `on_state`, `on_log`); `call_ui` takes a `FnOnce` for the UI's answer.
* Values are JSON text; the example uses serde_json.
* `GpuContext::gl()` / `gl_with_loader(...)` and `render_gl` draw with Skia on
  the program's OpenGL context; `render_rgba` / `render_png` on the CPU.
* `Config::js_thread` = `JsThread::On` runs the bundle's JS on a thread of its
  own (React Native's model; the default is `Off`, as in the C API): `tick`
  never waits for JS, `Config::on_js_ready` wakes a loop that sleeps while
  idle (the example posts a GLFW empty event), and `tick_sync` / `sync` wait
  for JS when a change must show on this frame. `debug_stall_js` keeps JS
  busy, to see the frames go on.

`build.rs` links `libjsui` from `JSUI_LIB_DIR`, or from `../../build` (this
repository built with `-DJSUI_BUILD_SHARED=ON`), and puts that directory in the
rpath of the crate's examples and tests. A program depending on the crate adds
its own rpath (`DEP_JSUI_LIB_DIR` has the directory) or ships the library next
to its executable.

```sh
cargo test                                            # needs bindings/ui/dist/bundle.js
cargo run --release --example quests                  # the quest log, see ../README.md
cargo run --release --example quests -- --selftest --screenshot quests.png
```
