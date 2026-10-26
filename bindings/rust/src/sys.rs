//! Raw declarations of the C API in `include/jsui/jsui.h`. Everything here is
//! `unsafe`; the safe wrapper in the crate root is built on it. Only the parts
//! the wrapper uses are declared, plus the common GPU and input calls; add
//! more from the header the same way.
#![allow(non_camel_case_types, missing_docs)]

use std::ffi::{c_char, c_double, c_float, c_int, c_uint, c_void};

#[repr(C)]
pub struct JsuiRuntime {
    _private: [u8; 0],
}
#[repr(C)]
pub struct JsuiGpuContext {
    _private: [u8; 0],
}

pub type JsuiLogLevel = c_int;
pub const JSUI_LOG_DEBUG: JsuiLogLevel = 0;
pub const JSUI_LOG_INFO: JsuiLogLevel = 1;
pub const JSUI_LOG_WARN: JsuiLogLevel = 2;
pub const JSUI_LOG_ERROR: JsuiLogLevel = 3;

pub type JsuiLogFn = Option<unsafe extern "C" fn(user: *mut c_void, level: JsuiLogLevel, message: *const c_char)>;
pub type JsuiLoadFn =
    Option<unsafe extern "C" fn(user: *mut c_void, rt: *mut JsuiRuntime, request_id: u64, url: *const c_char)>;
pub type JsuiMessageFn = Option<unsafe extern "C" fn(user: *mut c_void, name: *const c_char, json: *const c_char)>;
pub type JsuiStateFn = Option<unsafe extern "C" fn(user: *mut c_void, key: *const c_char, json: *const c_char)>;
pub type JsuiCallFn =
    Option<unsafe extern "C" fn(user: *mut c_void, call_id: u64, name: *const c_char, args_json: *const c_char)>;
pub type JsuiCallCancelFn = Option<unsafe extern "C" fn(user: *mut c_void, call_id: u64)>;
pub type JsuiCallResultFn =
    Option<unsafe extern "C" fn(user: *mut c_void, call_id: u64, ok: c_int, result: *const c_char)>;
/// `JsuiAudioFn` (see the header's Audio section); not wrapped yet.
pub type JsuiAudioFn = *const c_void;
/// `JsuiJsReadyFn`: runs on the JS thread (see the header's "JS thread").
pub type JsuiJsReadyFn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type JsuiGLGetProcFn = Option<unsafe extern "C" fn(user: *mut c_void, name: *const c_char) -> *mut c_void>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct JsuiConfig {
    pub width: c_int,
    pub height: c_int,
    pub pixel_ratio: c_float,
    pub base_url: *const c_char,
    pub dev_mode: c_int,
    pub log: JsuiLogFn,
    pub log_user: *mut c_void,
    pub load: JsuiLoadFn,
    pub load_user: *mut c_void,
    pub on_message: JsuiMessageFn,
    pub message_user: *mut c_void,
    pub on_state: JsuiStateFn,
    pub state_user: *mut c_void,
    pub on_call: JsuiCallFn,
    pub on_call_cancel: JsuiCallCancelFn,
    pub call_user: *mut c_void,
    pub js_memory_limit_mb: c_int,
    pub js_stack_size_kb: c_int,
    pub js_timeout_ms: c_int,
    pub on_audio: JsuiAudioFn,
    pub audio_user: *mut c_void,
    pub audio_disabled: c_int,
    pub js_thread: c_int,
    pub js_event_deadline_ms: c_int,
    pub js_sync_timeout_ms: c_int,
    pub on_js_ready: JsuiJsReadyFn,
    pub js_ready_user: *mut c_void,
}

pub const JSUI_JS_THREAD_OFF: c_int = 0;
pub const JSUI_JS_THREAD_ON: c_int = 1;
pub const JSUI_JS_THREAD_LOCKSTEP: c_int = 2;
pub const JSUI_TICK_SYNC_JS: c_int = 1;

pub type JsuiBufferType = c_int;
pub const JSUI_BUFFER_F32: JsuiBufferType = 0;
pub const JSUI_BUFFER_F64: JsuiBufferType = 1;
pub const JSUI_BUFFER_I32: JsuiBufferType = 2;
pub const JSUI_BUFFER_U32: JsuiBufferType = 3;
pub const JSUI_BUFFER_I16: JsuiBufferType = 4;
pub const JSUI_BUFFER_U16: JsuiBufferType = 5;
pub const JSUI_BUFFER_I8: JsuiBufferType = 6;
pub const JSUI_BUFFER_U8: JsuiBufferType = 7;

pub type JsuiMouseButton = c_int;
pub const JSUI_MOUSE_LEFT: JsuiMouseButton = 0;
pub const JSUI_MOUSE_MIDDLE: JsuiMouseButton = 1;
pub const JSUI_MOUSE_RIGHT: JsuiMouseButton = 2;

pub const JSUI_MOD_SHIFT: c_int = 1 << 0;
pub const JSUI_MOD_CTRL: c_int = 1 << 1;
pub const JSUI_MOD_ALT: c_int = 1 << 2;
pub const JSUI_MOD_META: c_int = 1 << 3;

pub const JSUI_HIT_POINTER: c_int = 1 << 0;
pub const JSUI_HIT_SCROLL: c_int = 1 << 1;

pub const JSUI_GL_TOP_LEFT_ORIGIN: c_int = 1 << 0;
pub const JSUI_GL_NO_CLEAR: c_int = 1 << 1;
pub const JSUI_GL_NO_RETAIN: c_int = 1 << 2;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct JsuiGLTarget {
    pub fbo: c_uint,
    pub texture: c_uint,
    pub format: c_uint,
    pub width: c_int,
    pub height: c_int,
    pub samples: c_int,
    pub stencil_bits: c_int,
    pub flags: c_int,
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct JsuiRect {
    pub x: c_int,
    pub y: c_int,
    pub width: c_int,
    pub height: c_int,
}

extern "C" {
    pub fn jsui_config_init(cfg: *mut JsuiConfig);
    pub fn jsui_create(cfg: *const JsuiConfig) -> *mut JsuiRuntime;
    pub fn jsui_destroy(rt: *mut JsuiRuntime);

    pub fn jsui_load_bundle_source(
        rt: *mut JsuiRuntime,
        source: *const c_char,
        size: usize,
        name: *const c_char,
    ) -> c_int;
    pub fn jsui_load_bundle_url(rt: *mut JsuiRuntime, url: *const c_char) -> c_int;
    pub fn jsui_reload(rt: *mut JsuiRuntime);
    pub fn jsui_resource_complete(
        rt: *mut JsuiRuntime,
        request_id: u64,
        status: c_int,
        content_type: *const c_char,
        data: *const c_void,
        size: usize,
    );

    pub fn jsui_post_message(rt: *mut JsuiRuntime, name: *const c_char, json: *const c_char);
    pub fn jsui_set_state(rt: *mut JsuiRuntime, key: *const c_char, json: *const c_char) -> c_int;
    pub fn jsui_patch_state(rt: *mut JsuiRuntime, key: *const c_char, merge_patch_json: *const c_char) -> c_int;
    pub fn jsui_get_state(rt: *mut JsuiRuntime, key: *const c_char) -> *const c_char;
    pub fn jsui_resolve_call(rt: *mut JsuiRuntime, call_id: u64, json: *const c_char) -> c_int;
    pub fn jsui_reject_call(rt: *mut JsuiRuntime, call_id: u64, message: *const c_char) -> c_int;
    pub fn jsui_call_ui(
        rt: *mut JsuiRuntime,
        name: *const c_char,
        args_json: *const c_char,
        done: JsuiCallResultFn,
        user: *mut c_void,
    ) -> u64;

    pub fn jsui_set_buffer(
        rt: *mut JsuiRuntime,
        key: *const c_char,
        ty: JsuiBufferType,
        data: *const c_void,
        count: usize,
    ) -> c_int;
    pub fn jsui_map_buffer(rt: *mut JsuiRuntime, key: *const c_char, ty: JsuiBufferType, count: usize) -> *mut c_void;
    pub fn jsui_commit_buffer(rt: *mut JsuiRuntime, key: *const c_char) -> c_int;
    pub fn jsui_remove_buffer(rt: *mut JsuiRuntime, key: *const c_char) -> c_int;

    pub fn jsui_set_texture_pixels(
        rt: *mut JsuiRuntime,
        key: *const c_char,
        rgba: *const c_void,
        width: c_int,
        height: c_int,
        row_bytes: usize,
        flags: c_int,
    ) -> c_int;
    pub fn jsui_remove_texture(rt: *mut JsuiRuntime, key: *const c_char) -> c_int;

    pub fn jsui_register_font(
        rt: *mut JsuiRuntime,
        family: *const c_char,
        data: *const c_void,
        size: usize,
        weight: c_int,
        italic: c_int,
    ) -> c_int;

    pub fn jsui_set_viewport(rt: *mut JsuiRuntime, width: c_int, height: c_int, pixel_ratio: c_float);
    pub fn jsui_tick(rt: *mut JsuiRuntime, now_ms: c_double);
    pub fn jsui_tick_ex(rt: *mut JsuiRuntime, now_ms: c_double, flags: c_int);
    pub fn jsui_set_js_thread(rt: *mut JsuiRuntime, mode: c_int) -> c_int;
    pub fn jsui_js_thread(rt: *mut JsuiRuntime) -> c_int;
    pub fn jsui_sync(rt: *mut JsuiRuntime, timeout_ms: c_double) -> c_int;
    pub fn jsui_debug_stall_js(rt: *mut JsuiRuntime, ms: c_double);
    pub fn jsui_pending_loads(rt: *mut JsuiRuntime) -> c_int;
    pub fn jsui_needs_redraw(rt: *mut JsuiRuntime) -> c_int;
    pub fn jsui_wants_frame(rt: *mut JsuiRuntime) -> c_int;
    pub fn jsui_next_timer(rt: *mut JsuiRuntime) -> c_double;
    pub fn jsui_render_rgba(rt: *mut JsuiRuntime, pixels: *mut c_void, width: c_int, height: c_int, row_bytes: usize);
    pub fn jsui_render_rgba_incremental(
        rt: *mut JsuiRuntime,
        pixels: *mut c_void,
        width: c_int,
        height: c_int,
        row_bytes: usize,
        dirty: *mut JsuiRect,
    ) -> c_int;
    pub fn jsui_render_png(rt: *mut JsuiRuntime, out_data: *mut *mut c_void, out_size: *mut usize) -> c_int;
    pub fn jsui_free(p: *mut c_void);

    pub fn jsui_gpu_create_gl(get_proc: JsuiGLGetProcFn, user: *mut c_void) -> *mut JsuiGpuContext;
    pub fn jsui_gpu_destroy(gpu: *mut JsuiGpuContext);
    pub fn jsui_gpu_context_lost(gpu: *mut JsuiGpuContext);
    pub fn jsui_gpu_purge(gpu: *mut JsuiGpuContext, cache_bytes: usize);
    pub fn jsui_render_gl(
        rt: *mut JsuiRuntime,
        gpu: *mut JsuiGpuContext,
        fbo: c_uint,
        width: c_int,
        height: c_int,
        flags: c_int,
    ) -> c_int;
    pub fn jsui_render_gl_target(rt: *mut JsuiRuntime, gpu: *mut JsuiGpuContext, target: *const JsuiGLTarget) -> c_int;

    pub fn jsui_set_event_time(rt: *mut JsuiRuntime, time_ms: c_double);
    pub fn jsui_hit_test(rt: *mut JsuiRuntime, x: c_float, y: c_float) -> c_int;
    pub fn jsui_send_mouse_move(rt: *mut JsuiRuntime, x: c_float, y: c_float) -> c_int;
    pub fn jsui_send_mouse_button(
        rt: *mut JsuiRuntime,
        x: c_float,
        y: c_float,
        button: JsuiMouseButton,
        down: c_int,
        mods: c_int,
    ) -> c_int;
    pub fn jsui_send_mouse_wheel(rt: *mut JsuiRuntime, x: c_float, y: c_float, dx: c_float, dy: c_float) -> c_int;
    pub fn jsui_send_mouse_leave(rt: *mut JsuiRuntime) -> c_int;
    pub fn jsui_send_touch(rt: *mut JsuiRuntime, id: c_int, phase: c_int, x: c_float, y: c_float) -> c_int;
    pub fn jsui_send_key(
        rt: *mut JsuiRuntime,
        key: *const c_char,
        code: *const c_char,
        down: c_int,
        repeat: c_int,
        mods: c_int,
    ) -> c_int;
    pub fn jsui_send_text(rt: *mut JsuiRuntime, utf8: *const c_char) -> c_int;
    pub fn jsui_send_gamepad_button(
        rt: *mut JsuiRuntime,
        pad: c_int,
        button: c_int,
        down: c_int,
        value: c_float,
    ) -> c_int;
    pub fn jsui_send_gamepad_axis(rt: *mut JsuiRuntime, pad: c_int, axis: c_int, value: c_float) -> c_int;
    pub fn jsui_set_gamepad_connected(rt: *mut JsuiRuntime, pad: c_int, connected: c_int, id: *const c_char);
    pub fn jsui_get_cursor(rt: *mut JsuiRuntime) -> *const c_char;
    pub fn jsui_last_error(rt: *mut JsuiRuntime) -> *const c_char;
}
