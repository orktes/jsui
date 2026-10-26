//! Rust bindings for [JSUI](https://github.com/orktes/jsui): UIs written in
//! React (React Native components), laid out with Yoga and drawn by Skia,
//! embedded in any program through libjsui's C API.
//!
//! ```no_run
//! let rt = jsui::Runtime::new(jsui::Config { width: 1280, height: 720, ..Default::default() }).unwrap();
//! rt.on_message(|_, name, json| println!("UI says {name}: {json}"));
//! rt.on_call(|cx, call| {
//!     if call.name == "add" {
//!         cx.resolve_call(call.id, r#"{"ok":true}"#);
//!     } else {
//!         cx.reject_call(call.id, "unknown call");
//!     }
//! });
//! rt.set_state("player", r#"{"name":"Ada","level":3}"#);
//! rt.load_bundle_file("ui/dist/bundle.js").unwrap();
//! loop {
//!     rt.tick(jsui::now_ms());
//!     // with the window's GL context current:
//!     // if rt.needs_redraw() { rt.render_gl(&gpu, 0, fb_w, fb_h, 0); swap buffers }
//! #   break;
//! }
//! ```
//!
//! The runtime is driven from one thread: a [`Runtime`] is neither `Send`
//! nor `Sync`, and every call (and every callback but
//! [`Config::on_js_ready`]) happens on the thread that made it. With
//! [`Config::js_thread`] = [`JsThread::On`] the bundle's JS runs on a
//! thread of its own inside the runtime, as React Native's does: ticks never
//! wait for it, and what JS does shows a tick or more later
//! ([`Context::tick_sync`] / [`Context::sync`] when it must not). Values cross the boundary as JSON text; use serde_json (or any
//! JSON library) on top. Callbacks run while the runtime is inside a call
//! (usually [`Context::tick`]) and get a [`Context`] to answer through.
//! A panic inside a callback aborts the process (it cannot unwind into C).

pub mod sys;

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fmt;
use std::ops::Deref;
use std::path::Path;
use std::ptr;
use std::time::Instant;

/// Errors reported by the runtime.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for Error {}

/// Options for [`Runtime::new`]; `Default` gives a 1280 x 720 viewport at
/// pixel ratio 1.
#[derive(Debug, Clone)]
pub struct Config {
    /// Viewport size in logical px.
    pub width: i32,
    pub height: i32,
    /// Device pixels per logical px (2 on a Retina display).
    pub pixel_ratio: f32,
    /// Resolves relative URLs (images, fonts, fetch); defaults to the bundle's URL.
    pub base_url: Option<String>,
    /// Poll the `jsui dev` server and hot-update on change; show an error overlay.
    pub dev_mode: bool,
    /// No sound at all (the built-in audio backend stays off).
    pub audio_disabled: bool,
    /// Where the bundle's JS runs. `Off` by default, as in the C API; a
    /// windowed app usually wants `On`.
    pub js_thread: JsThread,
    /// With `js_thread` `On`: called **on the JS thread** when JS has work
    /// for the next tick, to wake a loop that sleeps while idle (e.g.
    /// `glfwPostEmptyEvent`). Must be thread safe and must not panic.
    pub on_js_ready: Option<fn()>,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            width: 1280,
            height: 720,
            pixel_ratio: 1.0,
            base_url: None,
            dev_mode: false,
            audio_disabled: false,
            js_thread: JsThread::Off,
            on_js_ready: None,
        }
    }
}

impl std::fmt::Display for JsThread {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            JsThread::Off => "off",
            JsThread::On => "on",
            JsThread::Lockstep => "lockstep",
        })
    }
}

/// Where the bundle's JS runs ([`Config::js_thread`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum JsThread {
    /// On the thread that ticks: a tick runs the frame's JS before it returns.
    #[default]
    Off,
    /// On a thread of its own, as in React Native: ticks never wait for JS;
    /// native animations, scrolling and input go on while it is busy.
    On,
    /// On its own thread, but every tick waits for it: the frames of `Off` (tests).
    Lockstep,
}

/// Log severity of [`Context::on_log`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum LogLevel {
    Debug,
    Info,
    Warn,
    Error,
}

/// A `Host.call(name, args)` from the UI. Answer it exactly once, now or on
/// a later frame, with [`Context::resolve_call`] or [`Context::reject_call`].
#[derive(Debug, Clone, Copy)]
pub struct Call<'a> {
    pub id: u64,
    pub name: &'a str,
    /// The arguments as JSON text.
    pub args: &'a str,
}

/// Mouse buttons for [`Context::send_mouse_button`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Middle,
    Right,
}

/// Modifier bits for input events.
pub mod mods {
    pub const SHIFT: i32 = super::sys::JSUI_MOD_SHIFT;
    pub const CTRL: i32 = super::sys::JSUI_MOD_CTRL;
    pub const ALT: i32 = super::sys::JSUI_MOD_ALT;
    pub const META: i32 = super::sys::JSUI_MOD_META;
}

/// Flags for [`Context::render_gl`].
pub mod gl {
    /// Row 0 of the target is the top of the UI (textures sampled D3D style).
    pub const TOP_LEFT_ORIGIN: i32 = super::sys::JSUI_GL_TOP_LEFT_ORIGIN;
    /// Draw over the target's contents (a HUD over the game frame).
    pub const NO_CLEAR: i32 = super::sys::JSUI_GL_NO_CLEAR;
    /// Paint the whole frame every render instead of only what changed.
    pub const NO_RETAIN: i32 = super::sys::JSUI_GL_NO_RETAIN;
}

type MessageFn = Box<dyn FnMut(&Context, &str, &str)>;
type StateFn = Box<dyn FnMut(&Context, &str, Option<&str>)>;
type CallFn = Box<dyn FnMut(&Context, Call<'_>)>;
type CancelFn = Box<dyn FnMut(&Context, u64)>;
type LogFn = Box<dyn FnMut(LogLevel, &str)>;
type GlLoader = Box<dyn FnMut(&str) -> *const c_void>;

#[derive(Default)]
struct Handlers {
    message: RefCell<Option<MessageFn>>,
    state: RefCell<Option<StateFn>>,
    call: RefCell<Option<CallFn>>,
    cancel: RefCell<Option<CancelFn>>,
    log: RefCell<Option<LogFn>>,
}

/// A JSUI runtime: one bundle, one JS context, its shared state and its main
/// surface. Dereferences to [`Context`], which has every method. Dropping it
/// destroys the runtime.
pub struct Runtime {
    cx: Box<Context>,
}

/// The runtime's API, also handed to callbacks.
pub struct Context {
    rt: Cell<*mut sys::JsuiRuntime>,
    handlers: Handlers,
}

impl Deref for Runtime {
    type Target = Context;
    fn deref(&self) -> &Context {
        &self.cx
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        let rt = self.cx.rt.replace(ptr::null_mut());
        if !rt.is_null() {
            unsafe { sys::jsui_destroy(rt) }
        }
    }
}

fn cstring(s: &str) -> CString {
    CString::new(s).expect("jsui: string contains a NUL byte")
}

unsafe fn opt_str<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        Some(CStr::from_ptr(p).to_str().unwrap_or(""))
    }
}

unsafe fn str_or_empty<'a>(p: *const c_char) -> &'a str {
    opt_str(p).unwrap_or("")
}

unsafe extern "C" fn on_message_c(user: *mut c_void, name: *const c_char, json: *const c_char) {
    let cx = &*(user as *const Context);
    if let Ok(mut h) = cx.handlers.message.try_borrow_mut() {
        if let Some(f) = h.as_mut() {
            f(cx, str_or_empty(name), str_or_empty(json));
        }
    }
}

unsafe extern "C" fn on_state_c(user: *mut c_void, key: *const c_char, json: *const c_char) {
    let cx = &*(user as *const Context);
    if let Ok(mut h) = cx.handlers.state.try_borrow_mut() {
        if let Some(f) = h.as_mut() {
            f(cx, str_or_empty(key), opt_str(json));
        }
    }
}

unsafe extern "C" fn on_call_c(user: *mut c_void, id: u64, name: *const c_char, args: *const c_char) {
    let cx = &*(user as *const Context);
    let call = Call { id, name: str_or_empty(name), args: opt_str(args).unwrap_or("null") };
    match cx.handlers.call.try_borrow_mut() {
        Ok(mut h) => match h.as_mut() {
            Some(f) => f(cx, call),
            None => {
                cx.reject_call(id, &format!("no handler for '{}'", call.name));
            }
        },
        Err(_) => {
            cx.reject_call(id, "call handler busy");
        }
    }
}

unsafe extern "C" fn on_call_cancel_c(user: *mut c_void, id: u64) {
    let cx = &*(user as *const Context);
    if let Ok(mut h) = cx.handlers.cancel.try_borrow_mut() {
        if let Some(f) = h.as_mut() {
            f(cx, id);
        }
    }
}

unsafe extern "C" fn on_log_c(user: *mut c_void, level: sys::JsuiLogLevel, message: *const c_char) {
    let cx = &*(user as *const Context);
    let level = match level {
        sys::JSUI_LOG_DEBUG => LogLevel::Debug,
        sys::JSUI_LOG_INFO => LogLevel::Info,
        sys::JSUI_LOG_WARN => LogLevel::Warn,
        _ => LogLevel::Error,
    };
    let msg = str_or_empty(message);
    match cx.handlers.log.try_borrow_mut() {
        Ok(mut h) if h.is_some() => (h.as_mut().unwrap())(level, msg),
        _ => eprintln!("[jsui] {msg}"),
    }
}

unsafe extern "C" fn js_ready_c(user: *mut c_void) {
    // `user` is the `fn()` of Config::on_js_ready.
    let f = std::mem::transmute::<*mut c_void, fn()>(user);
    f();
}

type CallUiDone = Box<dyn FnOnce(Result<&str, &str>)>;

unsafe extern "C" fn call_ui_done_c(user: *mut c_void, _id: u64, ok: c_int, result: *const c_char) {
    let done = Box::from_raw(user as *mut CallUiDone);
    let text = str_or_empty(result);
    (*done)(if ok != 0 { Ok(text) } else { Err(text) });
}

impl Runtime {
    /// Creates a runtime. Load a bundle next, then tick it every frame.
    pub fn new(config: Config) -> Result<Runtime, Error> {
        let cx = Box::new(Context { rt: Cell::new(ptr::null_mut()), handlers: Handlers::default() });
        let user = &*cx as *const Context as *mut c_void;
        let base_url = config.base_url.as_deref().map(cstring);
        let mut cfg = unsafe {
            let mut cfg = std::mem::MaybeUninit::<sys::JsuiConfig>::zeroed();
            sys::jsui_config_init(cfg.as_mut_ptr());
            cfg.assume_init()
        };
        cfg.width = config.width;
        cfg.height = config.height;
        cfg.pixel_ratio = config.pixel_ratio;
        cfg.base_url = base_url.as_ref().map_or(ptr::null(), |s| s.as_ptr());
        cfg.dev_mode = config.dev_mode as c_int;
        cfg.audio_disabled = config.audio_disabled as c_int;
        cfg.js_thread = match config.js_thread {
            JsThread::Off => sys::JSUI_JS_THREAD_OFF,
            JsThread::On => sys::JSUI_JS_THREAD_ON,
            JsThread::Lockstep => sys::JSUI_JS_THREAD_LOCKSTEP,
        };
        if let Some(f) = config.on_js_ready {
            cfg.on_js_ready = Some(js_ready_c);
            cfg.js_ready_user = f as *const () as *mut c_void;
        }
        cfg.log = Some(on_log_c);
        cfg.log_user = user;
        cfg.on_message = Some(on_message_c);
        cfg.message_user = user;
        cfg.on_state = Some(on_state_c);
        cfg.state_user = user;
        cfg.on_call = Some(on_call_c);
        cfg.on_call_cancel = Some(on_call_cancel_c);
        cfg.call_user = user;
        let rt = unsafe { sys::jsui_create(&cfg) };
        if rt.is_null() {
            return Err(Error("jsui_create failed".into()));
        }
        cx.rt.set(rt);
        Ok(Runtime { cx })
    }
}

impl Context {
    /// The raw handle, for C API calls this crate does not wrap.
    pub fn as_ptr(&self) -> *mut sys::JsuiRuntime {
        self.rt.get()
    }

    // ---------------------------------------------------------- callbacks

    /// `Host.postMessage(name, payload)` from the UI; payload as JSON text.
    pub fn on_message(&self, f: impl FnMut(&Context, &str, &str) + 'static) {
        *self.handlers.message.borrow_mut() = Some(Box::new(f));
    }
    /// The UI changed a shared-state key (`HostState.set`); `None` when it removed it.
    pub fn on_state(&self, f: impl FnMut(&Context, &str, Option<&str>) + 'static) {
        *self.handlers.state.borrow_mut() = Some(Box::new(f));
    }
    /// `Host.call(name, args)` from the UI. Without a handler calls are rejected.
    pub fn on_call(&self, f: impl FnMut(&Context, Call<'_>) + 'static) {
        *self.handlers.call.borrow_mut() = Some(Box::new(f));
    }
    /// A call ended unanswered (its JS timeout, or a reload).
    pub fn on_call_cancel(&self, f: impl FnMut(&Context, u64) + 'static) {
        *self.handlers.cancel.borrow_mut() = Some(Box::new(f));
    }
    /// Runtime and `console.*` output. Default: stderr.
    pub fn on_log(&self, f: impl FnMut(LogLevel, &str) + 'static) {
        *self.handlers.log.borrow_mut() = Some(Box::new(f));
    }

    // ------------------------------------------------------------ content

    /// Evaluates a bundle (`npx jsui build app.jsx`); `name` shows in stack traces.
    pub fn load_bundle_source(&self, source: &str, name: &str) -> Result<(), Error> {
        self.load_bundle_bytes(source.as_bytes(), name)
    }
    /// Evaluates a bundle given as bytes: JS source or a precompiled `jsui build` bundle (bytecode).
    pub fn load_bundle_bytes(&self, bundle: &[u8], name: &str) -> Result<(), Error> {
        let name = cstring(name);
        let r =
            unsafe { sys::jsui_load_bundle_source(self.as_ptr(), bundle.as_ptr().cast(), bundle.len(), name.as_ptr()) };
        if r == 0 {
            Ok(())
        } else {
            Err(Error(self.last_error().unwrap_or_else(|| "bundle failed to load".into())))
        }
    }
    /// Reads and evaluates a bundle file; relative URLs then resolve next to it.
    pub fn load_bundle_file(&self, path: impl AsRef<Path>) -> Result<(), Error> {
        let path = path.as_ref();
        // Bytes, not a String: `jsui build` bundles are precompiled (binary).
        let bundle = std::fs::read(path).map_err(|e| Error(format!("{}: {e}", path.display())))?;
        let abs = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
        self.load_bundle_bytes(&bundle, &format!("file://{}", abs.display()))
    }
    /// Loads a bundle through the loader (file path, file:// or http:// URL, e.g. `jsui dev`).
    pub fn load_bundle_url(&self, url: &str) -> Result<(), Error> {
        let url = cstring(url);
        match unsafe { sys::jsui_load_bundle_url(self.as_ptr(), url.as_ptr()) } {
            0 => Ok(()),
            _ => Err(Error(self.last_error().unwrap_or_else(|| "bundle failed to load".into()))),
        }
    }
    /// Tears down the JS context and loads the last bundle again.
    pub fn reload(&self) {
        unsafe { sys::jsui_reload(self.as_ptr()) }
    }
    /// The last uncaught JS error (message and stack).
    pub fn last_error(&self) -> Option<String> {
        unsafe { opt_str(sys::jsui_last_error(self.as_ptr())).map(str::to_owned) }
    }

    // --------------------------------------------------- talking to the UI

    /// Sets shared state `key` to JSON text (`useHostState(key)` in the UI).
    /// Returns whether the value changed. `json` must be valid JSON.
    pub fn set_state(&self, key: &str, json: &str) -> bool {
        let (k, j) = (cstring(key), cstring(json));
        unsafe { sys::jsui_set_state(self.as_ptr(), k.as_ptr(), j.as_ptr()) == 1 }
    }
    /// Applies an RFC 7386 JSON merge patch to shared state `key`.
    pub fn patch_state(&self, key: &str, merge_patch_json: &str) -> bool {
        let (k, j) = (cstring(key), cstring(merge_patch_json));
        unsafe { sys::jsui_patch_state(self.as_ptr(), k.as_ptr(), j.as_ptr()) == 1 }
    }
    /// Removes shared state `key`.
    pub fn remove_state(&self, key: &str) -> bool {
        let k = cstring(key);
        unsafe { sys::jsui_set_state(self.as_ptr(), k.as_ptr(), ptr::null()) == 1 }
    }
    /// The current JSON text of shared state `key`.
    pub fn get_state(&self, key: &str) -> Option<String> {
        let k = cstring(key);
        unsafe { opt_str(sys::jsui_get_state(self.as_ptr(), k.as_ptr())).map(str::to_owned) }
    }
    /// Sends `name` with a JSON payload to `Host.onMessage(name, cb)` / `useHostMessage`.
    pub fn post_message(&self, name: &str, json: &str) {
        let (n, j) = (cstring(name), cstring(json));
        unsafe { sys::jsui_post_message(self.as_ptr(), n.as_ptr(), j.as_ptr()) }
    }
    /// Answers a [`Call`]: its promise resolves with the parsed JSON.
    /// `false` if the call is unknown, already settled or `json` is invalid.
    pub fn resolve_call(&self, id: u64, json: &str) -> bool {
        let j = cstring(json);
        unsafe { sys::jsui_resolve_call(self.as_ptr(), id, j.as_ptr()) == 0 }
    }
    /// Rejects a [`Call`] with an `Error(message)`.
    pub fn reject_call(&self, id: u64, message: &str) -> bool {
        let m = cstring(message);
        unsafe { sys::jsui_reject_call(self.as_ptr(), id, m.as_ptr()) == 0 }
    }
    /// Runs the UI's `Host.handle(name, fn)` handler; `done` gets its result
    /// as JSON text (or the error) inside a later tick. Returns the call id.
    pub fn call_ui(&self, name: &str, args_json: &str, done: impl FnOnce(Result<&str, &str>) + 'static) -> u64 {
        let (n, a) = (cstring(name), cstring(args_json));
        let boxed: Box<CallUiDone> = Box::new(Box::new(done));
        unsafe {
            sys::jsui_call_ui(self.as_ptr(), n.as_ptr(), a.as_ptr(), Some(call_ui_done_c), Box::into_raw(boxed).cast())
        }
    }
    /// Sets frame-data buffer `key` (a `Float32Array` in the UI: `useHostBufferValue`).
    pub fn set_buffer_f32(&self, key: &str, data: &[f32]) -> bool {
        let k = cstring(key);
        unsafe {
            sys::jsui_set_buffer(self.as_ptr(), k.as_ptr(), sys::JSUI_BUFFER_F32, data.as_ptr().cast(), data.len()) == 1
        }
    }
    /// Removes frame-data buffer `key`.
    pub fn remove_buffer(&self, key: &str) -> bool {
        let k = cstring(key);
        unsafe { sys::jsui_remove_buffer(self.as_ptr(), k.as_ptr()) == 1 }
    }
    /// Shows RGBA8 pixels (sRGB, unpremultiplied rows of `width * 4` bytes)
    /// as host texture `key` (`<HostTexture source="key">`).
    pub fn set_texture_pixels(&self, key: &str, rgba: &[u8], width: i32, height: i32) -> bool {
        assert!(rgba.len() >= (width.max(0) as usize) * (height.max(0) as usize) * 4, "jsui: texture data too small");
        let k = cstring(key);
        unsafe {
            sys::jsui_set_texture_pixels(self.as_ptr(), k.as_ptr(), rgba.as_ptr().cast(), width, height, 0, 0) == 1
        }
    }
    /// Registers a font (TTF, OTF, TTC, WOFF, WOFF2) under `family` (`""`: its own name).
    pub fn register_font(&self, family: &str, data: &[u8]) -> Result<(), Error> {
        let f = cstring(family);
        match unsafe { sys::jsui_register_font(self.as_ptr(), f.as_ptr(), data.as_ptr().cast(), data.len(), 0, -1) } {
            0 => Ok(()),
            _ => Err(Error("not a font".into())),
        }
    }

    // ---------------------------------------------------------- frame loop

    /// The viewport in logical px and the device pixel ratio.
    pub fn set_viewport(&self, width: i32, height: i32, pixel_ratio: f32) {
        unsafe { sys::jsui_set_viewport(self.as_ptr(), width, height, pixel_ratio) }
    }
    /// Advances to `now_ms` (monotonic, e.g. [`now_ms`]): timers, loads,
    /// animation frames, layout. Callbacks run in here.
    pub fn tick(&self, now_ms: f64) {
        unsafe { sys::jsui_tick(self.as_ptr(), now_ms) }
    }
    /// A tick whose JS lands on this frame (`JSUI_TICK_SYNC_JS`): with JS on
    /// its own thread it waits (up to 250 ms) for JS to handle the frame and
    /// what was sent before it. Scene cuts, screenshots, state that must not lag.
    pub fn tick_sync(&self, now_ms: f64) {
        unsafe { sys::jsui_tick_ex(self.as_ptr(), now_ms, sys::JSUI_TICK_SYNC_JS) }
    }
    /// With JS on its own thread: waits up to `timeout_ms` (negative: no
    /// limit) for JS to handle what was sent to it, applies it and lays out
    /// (callbacks run in here). `true` when JS is left idle, `false` on
    /// timeout. Without the thread: `true` at once.
    pub fn sync(&self, timeout_ms: f64) -> bool {
        unsafe { sys::jsui_sync(self.as_ptr(), timeout_ms) != 0 }
    }
    /// The mode the runtime runs in (`Off` where the build has no threads).
    pub fn js_thread(&self) -> JsThread {
        match unsafe { sys::jsui_js_thread(self.as_ptr()) } {
            sys::JSUI_JS_THREAD_ON => JsThread::On,
            sys::JSUI_JS_THREAD_LOCKSTEP => JsThread::Lockstep,
            _ => JsThread::Off,
        }
    }
    /// Debugging: keeps JS busy for `ms` (`jsui_debug_stall_js`). With the JS
    /// thread the ticks go on meanwhile; without, this call takes `ms`.
    pub fn debug_stall_js(&self, ms: f64) {
        unsafe { sys::jsui_debug_stall_js(self.as_ptr(), ms) }
    }
    /// The last tick changed what the UI would paint.
    pub fn needs_redraw(&self) -> bool {
        unsafe { sys::jsui_needs_redraw(self.as_ptr()) != 0 }
    }
    /// Something is moving: keep ticking at display rate even without input.
    pub fn wants_frame(&self) -> bool {
        unsafe { sys::jsui_wants_frame(self.as_ptr()) != 0 }
    }
    /// When JS next has a timer due, in the clock of `tick`'s `now_ms`
    /// (infinity: none): when an idle host (`wants_frame` false) ticks again.
    pub fn next_timer(&self) -> f64 {
        unsafe { sys::jsui_next_timer(self.as_ptr()) }
    }
    /// Resource loads still in flight (bundle, images, fonts, fetch).
    pub fn pending_loads(&self) -> i32 {
        unsafe { sys::jsui_pending_loads(self.as_ptr()) }
    }
    /// Paints the frame with Skia on the GPU into GL framebuffer `fbo` (0: the
    /// window's) of `width` x `height` device px. Needs `gpu`'s GL context current.
    pub fn render_gl(&self, gpu: &GpuContext, fbo: u32, width: i32, height: i32, flags: i32) -> bool {
        unsafe { sys::jsui_render_gl(self.as_ptr(), gpu.ptr, fbo, width, height, flags) == 0 }
    }
    /// Paints the frame on the CPU into premultiplied RGBA8 `pixels`
    /// (`width * height * 4` bytes).
    pub fn render_rgba(&self, pixels: &mut [u8], width: i32, height: i32) {
        assert!(pixels.len() >= (width.max(0) as usize) * (height.max(0) as usize) * 4, "jsui: pixel buffer too small");
        unsafe { sys::jsui_render_rgba(self.as_ptr(), pixels.as_mut_ptr().cast(), width, height, width as usize * 4) }
    }
    /// The frame as a PNG (CPU raster).
    pub fn render_png(&self) -> Option<Vec<u8>> {
        let mut data: *mut c_void = ptr::null_mut();
        let mut size = 0usize;
        unsafe {
            if sys::jsui_render_png(self.as_ptr(), &mut data, &mut size) != 0 || data.is_null() {
                return None;
            }
            let v = std::slice::from_raw_parts(data as *const u8, size).to_vec();
            sys::jsui_free(data);
            Some(v)
        }
    }

    // --------------------------------------------------------------- input
    // Coordinates are logical px from the viewport's top-left. Each returns
    // whether the UI consumed the event; when it did not, it is the host's.

    pub fn send_mouse_move(&self, x: f32, y: f32) -> bool {
        unsafe { sys::jsui_send_mouse_move(self.as_ptr(), x, y) != 0 }
    }
    pub fn send_mouse_button(&self, x: f32, y: f32, button: MouseButton, down: bool, mods: i32) -> bool {
        let b = match button {
            MouseButton::Left => sys::JSUI_MOUSE_LEFT,
            MouseButton::Middle => sys::JSUI_MOUSE_MIDDLE,
            MouseButton::Right => sys::JSUI_MOUSE_RIGHT,
        };
        unsafe { sys::jsui_send_mouse_button(self.as_ptr(), x, y, b, down as c_int, mods) != 0 }
    }
    pub fn send_mouse_wheel(&self, x: f32, y: f32, dx: f32, dy: f32) -> bool {
        unsafe { sys::jsui_send_mouse_wheel(self.as_ptr(), x, y, dx, dy) != 0 }
    }
    pub fn send_mouse_leave(&self) -> bool {
        unsafe { sys::jsui_send_mouse_leave(self.as_ptr()) != 0 }
    }
    /// A key in DOM terms: `key` "a" / "Enter" / "ArrowLeft", `code` "KeyA" / "Enter".
    pub fn send_key(&self, key: Option<&str>, code: Option<&str>, down: bool, repeat: bool, mods: i32) -> bool {
        let k = key.map(cstring);
        let c = code.map(cstring);
        unsafe {
            sys::jsui_send_key(
                self.as_ptr(),
                k.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                c.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                down as c_int,
                repeat as c_int,
                mods,
            ) != 0
        }
    }
    /// Typed text (after the key event).
    pub fn send_text(&self, text: &str) -> bool {
        let t = cstring(text);
        unsafe { sys::jsui_send_text(self.as_ptr(), t.as_ptr()) != 0 }
    }
    /// Gamepad button in the W3C standard mapping (0 A, 1 B, 12-15 D-pad, ...).
    pub fn send_gamepad_button(&self, pad: i32, button: i32, down: bool, value: f32) -> bool {
        unsafe { sys::jsui_send_gamepad_button(self.as_ptr(), pad, button, down as c_int, value) != 0 }
    }
    pub fn send_gamepad_axis(&self, pad: i32, axis: i32, value: f32) -> bool {
        unsafe { sys::jsui_send_gamepad_axis(self.as_ptr(), pad, axis, value) != 0 }
    }
    /// What the UI would capture at a point (`sys::JSUI_HIT_*` bits); 0 passes through.
    pub fn hit_test(&self, x: f32, y: f32) -> i32 {
        unsafe { sys::jsui_hit_test(self.as_ptr(), x, y) }
    }
    /// The CSS cursor the UI wants ("default", "pointer", "text", ...).
    pub fn cursor(&self) -> String {
        unsafe { opt_str(sys::jsui_get_cursor(self.as_ptr())).unwrap_or("default").to_owned() }
    }
}

/// A Skia GPU context on the program's OpenGL context. Create it, use it and
/// drop it with that GL context current.
pub struct GpuContext {
    ptr: *mut sys::JsuiGpuContext,
    _loader: Option<Box<GlLoader>>,
}

unsafe extern "C" fn gl_loader_c(user: *mut c_void, name: *const c_char) -> *mut c_void {
    let f = &mut *(user as *mut GlLoader);
    f(str_or_empty(name)) as *mut c_void
}

impl GpuContext {
    /// Skia Ganesh on the current GL context, GL functions from the
    /// platform's default loader (GLX / EGL, OpenGL.framework, WGL).
    /// `None` without a usable context or in a library built without GPU.
    pub fn gl() -> Option<GpuContext> {
        let ptr = unsafe { sys::jsui_gpu_create_gl(None, ptr::null_mut()) };
        (!ptr.is_null()).then_some(GpuContext { ptr, _loader: None })
    }
    /// Same with the window library's loader, e.g. `|name| window.get_proc_address(name)`.
    pub fn gl_with_loader(loader: impl FnMut(&str) -> *const c_void + 'static) -> Option<GpuContext> {
        let mut boxed: Box<GlLoader> = Box::new(Box::new(loader));
        let user = &mut *boxed as *mut GlLoader as *mut c_void;
        let ptr = unsafe { sys::jsui_gpu_create_gl(Some(gl_loader_c), user) };
        (!ptr.is_null()).then_some(GpuContext { ptr, _loader: Some(boxed) })
    }
    /// The GL context is gone: drop GPU objects without calling GL.
    pub fn context_lost(&self) {
        unsafe { sys::jsui_gpu_context_lost(self.ptr) }
    }
    pub fn as_ptr(&self) -> *mut sys::JsuiGpuContext {
        self.ptr
    }
}

impl Drop for GpuContext {
    fn drop(&mut self) {
        unsafe { sys::jsui_gpu_destroy(self.ptr) }
    }
}

/// Milliseconds on a monotonic clock, for [`Context::tick`].
pub fn now_ms() -> f64 {
    thread_local!(static START: Instant = Instant::now());
    START.with(|s| s.elapsed().as_secs_f64() * 1000.0)
}
