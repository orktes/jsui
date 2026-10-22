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
/* Same as EMSCRIPTEN_KEEPALIVE; spelled out because <emscripten.h> must not
 * be included inside this header's extern "C" block. */
#  define JSUI_API __attribute__((used, visibility("default")))
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

    /* JS engine limits (0: default). The watchdog interrupts any single
     * native->JS call (an event, a timer, a frame callback) that runs longer
     * than js_timeout_ms and reports it as an error; negative disables it. */
    int js_memory_limit_mb; /* default 256 */
    int js_stack_size_kb;   /* default 1024 (512 on Windows, whose threads get 1 MB stacks) */
    int js_timeout_ms;      /* default 5000 */
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
/* Number of resource loads (bundle, fetch, images, fonts) still in flight.
 * Headless hosts keep ticking until this reaches 0 before capturing. */
JSUI_API int jsui_pending_loads(JsuiRuntime* rt);
/* Non-zero when the last tick changed what would be painted. */
JSUI_API int jsui_needs_redraw(JsuiRuntime* rt);
/* Paint into a caller-owned RGBA8888 premultiplied buffer of
 * width*height device pixels. The buffer is cleared to transparent first. */
JSUI_API void jsui_render_rgba(JsuiRuntime* rt, void* pixels, int width, int height, size_t row_bytes);
/* A rectangle in device pixels. */
typedef struct JsuiRect {
    int x, y, width, height;
} JsuiRect;
/* Incremental variant of jsui_render_rgba for hosts that keep the buffer
 * between frames (e.g. a texture staging buffer): `pixels` must still hold
 * the frame this runtime last rendered into it, and only pixels that changed
 * since then are repainted. Rendering into a different buffer or size (or a
 * jsui_render_rgba call in between) falls back to a full repaint.
 * `dirty` (optional) receives the repainted rect, empty when nothing changed,
 * so the host can upload just that region. Returns 1 if any pixel was
 * repainted, 0 otherwise. */
JSUI_API int jsui_render_rgba_incremental(JsuiRuntime* rt, void* pixels, int width, int height, size_t row_bytes,
                                          JsuiRect* dirty);
/* ------------------------------------------------------------ GPU (OpenGL)
 *
 * Optional GPU backend: Skia Ganesh draws the frame straight into the
 * embedder's OpenGL / OpenGL ES / WebGL framebuffer or texture, so there is no
 * CPU raster and no texture upload. The CPU path above stays the default and
 * the reference for pixel tests; GPU output matches it within anti-aliasing /
 * blur rounding. Available when the library was built with JSUI_GPU (desktop
 * builds by default, the WebGL WASM variant); otherwise jsui_gpu_create_gl()
 * returns NULL and the host keeps using jsui_render_rgba().
 *
 *     // once, with the game's GL context current:
 *     JsuiGpuContext* gpu = jsui_gpu_create_gl(my_get_proc, NULL);  // NULL loader: platform default
 *     // per frame, GL context current:
 *     jsui_tick(rt, now_ms);
 *     jsui_render_gl(rt, gpu, 0, fb_width, fb_height, JSUI_GL_NO_CLEAR);  // UI over the game frame
 *     // restore the GL state the engine relies on, then swap buffers
 *     jsui_gpu_destroy(gpu);  // GL context still current
 *
 * Rules:
 * - Every jsui_gpu_* / jsui_render_gl* call needs the GL context the
 *   JsuiGpuContext was created with (or one sharing its objects) current.
 * - Skia assumes nothing about GL state on entry (the context's cached state
 *   is reset at the start of every render), but it leaves its own state
 *   behind: bound framebuffer, program, textures, blend, scissor, viewport,
 *   vertex arrays, pixel store... Engines that cache GL state must re-apply it
 *   afterwards.
 * - One JsuiGpuContext may serve any number of runtimes. Decoded images and
 *   shadow masks stay in the runtime as CPU images; Ganesh uploads each once
 *   and keeps the texture cached in the context (keyed by image), so nothing
 *   in a runtime is tied to a GPU context.
 * - The GPU path always repaints the whole target (there is no incremental
 *   mode: the target's previous contents are not assumed to be preserved).
 */
typedef struct JsuiGpuContext JsuiGpuContext;

/* Returns the address of GL function `name` (e.g. SDL_GL_GetProcAddress,
 * glfwGetProcAddress, eglGetProcAddress), or NULL. */
typedef void* (*JsuiGLGetProcFn)(void* user, const char* name);

/* Creates a Ganesh GL context on the GL context that is current. `get_proc`
 * NULL uses the platform default (Linux: EGL or GLX, whichever context is
 * current; macOS: OpenGL.framework; Windows: WGL; browser: the WebGL context
 * made current through emscripten). Desktop GL, GLES 2/3 and WebGL 1/2 are
 * detected from GL_VERSION. Returns NULL without a usable GL context or when
 * the library was built without JSUI_GPU. */
JSUI_API JsuiGpuContext* jsui_gpu_create_gl(JsuiGLGetProcFn get_proc, void* user);
/* Frees the context and its GPU resources. Needs the GL context current,
 * unless jsui_gpu_context_lost() was called first. */
JSUI_API void jsui_gpu_destroy(JsuiGpuContext* gpu);
/* The embedder changed GL state behind JSUI's back between two draws of the
 * same frame (renders already reset at their start; this is for hosts that
 * also issue other Skia/GL work through the same context). */
JSUI_API void jsui_gpu_reset(JsuiGpuContext* gpu);
/* The GL context was lost (webglcontextlost, device reset, the window's
 * context was destroyed): drop every GPU object without calling GL. The next
 * render recreates the context through the same loader from whichever GL
 * context is current then; jsui_gpu_destroy() is also fine. */
JSUI_API void jsui_gpu_context_lost(JsuiGpuContext* gpu);
/* Frees every cached GPU resource not in use right now (image textures,
 * glyph atlases' spare pages, scratch layers); they are recreated on demand.
 * For memory pressure. Ganesh otherwise trims its cache lazily (LRU) to its
 * budget, which `cache_bytes` > 0 sets (default 256 MB). */
JSUI_API void jsui_gpu_purge(JsuiGpuContext* gpu, size_t cache_bytes);

enum {
    /* Row 0 of the target is the top of the UI. Use it for textures that the
     * engine samples with v=0 at the top (D3D-style UVs). Default: GL's
     * bottom-left origin, which is upright when drawn to the screen (the
     * default framebuffer, or an FBO blitted / sampled the GL way). */
    JSUI_GL_TOP_LEFT_ORIGIN = 1 << 0,
    /* Draw over the target's current contents (premultiplied source-over),
     * e.g. the HUD on top of the game frame. Default: clear to transparent. */
    JSUI_GL_NO_CLEAR = 1 << 1
};

typedef struct JsuiGLTarget {
    unsigned int fbo;       /* framebuffer object to draw into; 0 = default framebuffer */
    unsigned int texture;   /* non-zero: draw into this GL_TEXTURE_2D instead (fbo is ignored) */
    unsigned int format;    /* sized internal format: 0 / GL_RGBA8 (0x8058), GL_BGRA8_EXT (0x93A1),
                               GL_SRGB8_ALPHA8 (0x8C43), GL_RGB8 (0x8051) for fbo, ... */
    int width, height;      /* device px */
    int samples;            /* MSAA sample count of fbo (0 or 1: none) */
    int stencil_bits;       /* stencil bits of fbo (0: none; Skia then avoids stencil techniques) */
    int flags;              /* JSUI_GL_* */
} JsuiGLTarget;

/* Paints the current frame into framebuffer `fbo` (0: default framebuffer,
 * GL_RGBA8 colour, no stencil / MSAA assumed) at width x height device px.
 * Like jsui_render_rgba it lays out first and clears needs_redraw. Returns 0
 * on success, -1 without a usable GPU context or target. */
JSUI_API int jsui_render_gl(JsuiRuntime* rt, JsuiGpuContext* gpu, unsigned int fbo, int width, int height, int flags);
/* Same with every target parameter (textures, MSAA, stencil, formats). */
JSUI_API int jsui_render_gl_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiGLTarget* target);

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

/* ------------------------------------------------------------- Clipboard
 *
 * TextInput copy / cut / paste (Ctrl or Cmd + C / X / V) and the JS
 * Clipboard API use these callbacks. Without them (or with NULL ones) the
 * runtime keeps an in-process clipboard. `get` returns UTF-8 text (NULL:
 * empty) owned by the embedder that must stay valid until the next `get`
 * call; the runtime copies it immediately.
 */
typedef const char* (*JsuiClipboardGetFn)(void* user);
typedef void (*JsuiClipboardSetFn)(void* user, const char* utf8);
JSUI_API void jsui_set_clipboard_callbacks(JsuiRuntime* rt, JsuiClipboardGetFn get, JsuiClipboardSetFn set,
                                           void* user);

/* The cursor the UI wants (CSS cursor name, e.g. "default", "pointer", "text"). */
JSUI_API const char* jsui_get_cursor(JsuiRuntime* rt);

/* --------------------------------------------------------------- Errors */

/* Last uncaught JS error (message + stack), or NULL. Valid until the next call into the runtime. */
JSUI_API const char* jsui_last_error(JsuiRuntime* rt);

#ifdef __cplusplus
}
#endif

#endif /* JSUI_H */
