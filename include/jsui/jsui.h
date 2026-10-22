/*
 * JSUI embedder API.
 *
 * A JSUI runtime runs a JavaScript (React) bundle inside QuickJS, lays the
 * resulting view tree out with Yoga and paints it with Skia. The embedder
 * (a game engine, a test tool, a browser page via WASM) owns the main loop:
 *
 *     JsuiConfig cfg; jsui_config_init(&cfg);
 *     cfg.width = 1280; cfg.height = 720; cfg.pixel_ratio = 1.0f;
 *     JsuiRuntime* rt = jsui_create(&cfg);
 *     jsui_load_bundle_url(rt, "http://localhost:5173/bundle.js");   // dev mode
 *     for (;;) {
 *         jsui_send_mouse_move(rt, x, y);                 // forward input
 *         jsui_tick(rt, now_ms);                          // timers, JS, layout
 *         if (jsui_needs_redraw(rt))
 *             jsui_render_rgba(rt, pixels, w, h, w * 4);  // paint
 *     }
 *     jsui_destroy(rt);
 *
 * All functions must be called from the thread that created the runtime.
 * Coordinates are in logical pixels (CSS px / React Native dp); the runtime
 * multiplies by pixel_ratio when rasterising.
 */
#ifndef JSUI_H
#define JSUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(JSUI_SHARED)
#  ifdef JSUI_BUILDING
#    define JSUI_API __declspec(dllexport)
#  else
#    define JSUI_API __declspec(dllimport)
#  endif
#elif defined(__EMSCRIPTEN__)
#  include <emscripten/emscripten.h>
#  define JSUI_API EMSCRIPTEN_KEEPALIVE
#else
#  define JSUI_API __attribute__((visibility("default")))
#endif

typedef struct JsuiRuntime JsuiRuntime;

/* ------------------------------------------------------------------ Logging */

typedef enum JsuiLogLevel {
    JSUI_LOG_DEBUG = 0,
    JSUI_LOG_INFO = 1,
    JSUI_LOG_WARN = 2,
    JSUI_LOG_ERROR = 3
} JsuiLogLevel;

typedef void (*JsuiLogFn)(void* user, JsuiLogLevel level, const char* message);

/* ------------------------------------------------------- Resource loading
 *
 * Every byte the runtime reads (the bundle, fetch() bodies, images, fonts)
 * goes through a loader. The built-in loader understands file://, relative
 * paths, http:// (for dev servers) and, in the browser build, anything the
 * browser's fetch() can reach. An embedder can replace it, e.g. to read from
 * a game's virtual file system or pak files.
 *
 * The loader is asynchronous: it is handed a request id and must eventually
 * call jsui_resource_complete() (from the runtime's thread, typically inside
 * or before the next jsui_tick()) exactly once with that id.
 */
typedef void (*JsuiLoadFn)(void* user, JsuiRuntime* rt, uint64_t request_id, const char* url);

JSUI_API void jsui_resource_complete(JsuiRuntime* rt, uint64_t request_id, int status,
                                     const char* content_type, const void* data, size_t size);

/* ------------------------------------------------------- Host messaging
 *
 * Game <-> UI data flow. JS calls `jsui.postMessage(name, payload)`; the
 * runtime JSON-encodes payload and calls this callback. The embedder sends
 * data to JS with jsui_post_message(); JS receives it via
 * `jsui.onMessage(name, cb)` (or the useHostMessage() hook).
 */
typedef void (*JsuiMessageFn)(void* user, const char* name, const char* json);

JSUI_API void jsui_post_message(JsuiRuntime* rt, const char* name, const char* json);

/* ---------------------------------------------------------------- Config */

typedef struct JsuiConfig {
    int width;              /* viewport size in logical px */
    int height;
    float pixel_ratio;      /* device pixels per logical px */
    const char* base_url;   /* resolves relative URLs; defaults to the bundle URL */
    int dev_mode;           /* 1: poll the dev server and reload on change, show error overlay */

    JsuiLogFn log;          /* NULL: print to stderr / console */
    void* log_user;
    JsuiLoadFn load;        /* NULL: built-in loader */
    void* load_user;
    JsuiMessageFn on_message;
    void* message_user;
} JsuiConfig;

JSUI_API void jsui_config_init(JsuiConfig* cfg);
JSUI_API JsuiRuntime* jsui_create(const JsuiConfig* cfg);
JSUI_API void jsui_destroy(JsuiRuntime* rt);

/* --------------------------------------------------------------- Content */

/* Evaluate a bundle given as source. `name` is used in stack traces and as
 * base URL when cfg.base_url is NULL. Returns 0 on success. */
JSUI_API int jsui_load_bundle_source(JsuiRuntime* rt, const char* source, size_t size, const char* name);
/* Load and evaluate a bundle via the loader (async; result visible after ticks). */
JSUI_API int jsui_load_bundle_url(JsuiRuntime* rt, const char* url);
/* Tear down the JS context and reload the last bundle (dev mode does this automatically). */
JSUI_API void jsui_reload(JsuiRuntime* rt);

/* Register a font face from memory. Data is copied. `weight` 100..900,
 * `italic` 0/1. The JS side can also load fonts with `jsui.loadFont(...)`. */
JSUI_API int jsui_register_font(JsuiRuntime* rt, const char* family, const void* data, size_t size,
                                int weight, int italic);

/* ------------------------------------------------------------ Frame loop */

JSUI_API void jsui_set_viewport(JsuiRuntime* rt, int width, int height, float pixel_ratio);
/* Advance time to `now_ms` (monotonic). Runs due timers, resolves loaded
 * resources, runs requestAnimationFrame callbacks, microtasks and layout. */
JSUI_API void jsui_tick(JsuiRuntime* rt, double now_ms);
/* Non-zero when the last tick changed what would be painted. */
JSUI_API int jsui_needs_redraw(JsuiRuntime* rt);
/* Paint into a caller-owned RGBA8888 premultiplied buffer of
 * width*height device pixels. The buffer is cleared to transparent first. */
JSUI_API void jsui_render_rgba(JsuiRuntime* rt, void* pixels, int width, int height, size_t row_bytes);
/* Encode the current frame as PNG (for tests / screenshots). Caller frees with jsui_free(). */
JSUI_API int jsui_render_png(JsuiRuntime* rt, void** out_data, size_t* out_size);
JSUI_API void jsui_free(void* p);

/* ----------------------------------------------------------------- Input
 *
 * Pointer coordinates are logical px relative to the viewport's top-left.
 * Each function returns non-zero if the UI consumed the event (it hit an
 * element with a handler, or a focused element handled the key), so the game
 * can decide whether to process the input itself.
 */
typedef enum JsuiMouseButton {
    JSUI_MOUSE_LEFT = 0,
    JSUI_MOUSE_MIDDLE = 1,
    JSUI_MOUSE_RIGHT = 2
} JsuiMouseButton;

enum {
    JSUI_MOD_SHIFT = 1 << 0,
    JSUI_MOD_CTRL = 1 << 1,
    JSUI_MOD_ALT = 1 << 2,
    JSUI_MOD_META = 1 << 3
};

JSUI_API int jsui_send_mouse_move(JsuiRuntime* rt, float x, float y);
JSUI_API int jsui_send_mouse_button(JsuiRuntime* rt, float x, float y, JsuiMouseButton button, int down, int mods);
JSUI_API int jsui_send_mouse_wheel(JsuiRuntime* rt, float x, float y, float dx, float dy);
JSUI_API int jsui_send_mouse_leave(JsuiRuntime* rt);

/* Touch: id is stable for the duration of a touch. phase: 0 begin, 1 move, 2 end, 3 cancel. */
JSUI_API int jsui_send_touch(JsuiRuntime* rt, int id, int phase, float x, float y);

/* Keys use DOM KeyboardEvent semantics: `key` is the logical key
 * ("a", "Enter", "ArrowLeft", "Escape", ...), `code` the physical key
 * ("KeyA", "Enter", ...). Either may be NULL. */
JSUI_API int jsui_send_key(JsuiRuntime* rt, const char* key, const char* code, int down, int repeat, int mods);
/* Text input (UTF-8), sent after the key event for printable characters / IME commits. */
JSUI_API int jsui_send_text(JsuiRuntime* rt, const char* utf8);

/* Gamepad, W3C "standard" mapping. Button indices: 0 A/Cross, 1 B/Circle,
 * 2 X/Square, 3 Y/Triangle, 4 LB, 5 RB, 6 LT, 7 RT, 8 Back, 9 Start,
 * 10 LS, 11 RS, 12 DpadUp, 13 DpadDown, 14 DpadLeft, 15 DpadRight, 16 Home.
 * Axes: 0 LX, 1 LY, 2 RX, 3 RY in [-1, 1].
 * The runtime maps D-pad / left stick to spatial focus navigation and
 * A/B to activate/back unless JS calls preventDefault(). */
JSUI_API int jsui_send_gamepad_button(JsuiRuntime* rt, int pad, int button, int down, float value);
JSUI_API int jsui_send_gamepad_axis(JsuiRuntime* rt, int pad, int axis, float value);
JSUI_API void jsui_set_gamepad_connected(JsuiRuntime* rt, int pad, int connected, const char* id);

/* The cursor the UI wants (CSS cursor name, e.g. "default", "pointer", "text"). */
JSUI_API const char* jsui_get_cursor(JsuiRuntime* rt);

/* --------------------------------------------------------------- Errors */

/* Last uncaught JS error (message + stack), or NULL. Valid until the next call into the runtime. */
JSUI_API const char* jsui_last_error(JsuiRuntime* rt);

#ifdef __cplusplus
}
#endif

#endif /* JSUI_H */
