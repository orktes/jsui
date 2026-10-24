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

/* Windows: define JSUI_SHARED when using jsui.dll (the prebuilt release
 * library); a static build needs nothing. */
#if defined(_WIN32) && defined(JSUI_SHARED)
#  ifdef JSUI_BUILDING
#    define JSUI_API __declspec(dllexport)
#  else
#    define JSUI_API __declspec(dllimport)
#  endif
#elif defined(_WIN32)
#  define JSUI_API
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
 * Game <-> UI events. JS calls `Host.postMessage(name, payload)`; the
 * runtime JSON-encodes payload and calls JsuiConfig.on_message. The embedder
 * sends events to JS with jsui_post_message(); JS receives them via
 * `Host.onMessage(name, cb)` (or the useHostMessage() hook). Messages are
 * fire-and-forget: a component that mounts later does not see earlier ones.
 * For data the UI shows (inventory, currency, settings) use shared state
 * below; for requests that need an answer use calls.
 */
typedef void (*JsuiMessageFn)(void* user, const char* name, const char* json);

JSUI_API void jsui_post_message(JsuiRuntime* rt, const char* name, const char* json);

/* ------------------------------------------------------- Shared state
 *
 * The runtime keeps the latest JSON value per key. It belongs to the runtime,
 * not to the JS context: it can be set before a bundle loads and survives
 * jsui_reload() and dev-mode reloads. In JS:
 *
 *     const inv = useHostState('inventory', { items: {} });  // re-renders on change
 *     HostState.get('inventory');  HostState.subscribe('inventory', cb);
 *
 * Only components subscribed to a changed key re-render. Changes made by the
 * embedder reach JS subscribers once per jsui_tick() (every key changed since
 * the last tick, in one React batch); reads always see the latest value, so a
 * component that mounts right after jsui_set_state() renders it immediately.
 * JS parses a key's JSON once per change and keeps unchanged sub-objects and
 * arrays identical (===) across updates, so memoized rows of a big list skip
 * re-rendering.
 *
 * Keys are UTF-8 strings ("inventory", "player.gold", ...). Setting a value
 * equal to the current one (same text, or text that parses to the same
 * value) is a no-op.
 */

/* Replace the value of `key` with the JSON text `json` (NULL removes the key;
 * JS then sees undefined, or the hook's fallback). Returns 1 if the value
 * changed, 0 if it was already equal, -1 for invalid JSON or a NULL key
 * (logged; the old value stays). */
JSUI_API int jsui_set_state(JsuiRuntime* rt, const char* key, const char* json);
/* Apply an RFC 7386 JSON Merge Patch to the value of `key`: objects merge
 * recursively, a null member deletes that member, anything else replaces.
 * E.g. {"gold": 120, "items": {"potion": {"count": 3}, "rusty_key": null}}.
 * Keep big collections as objects keyed by id (an array can only be replaced
 * as a whole). Member order is kept; new members are appended. A missing key
 * starts as null. Same return values as jsui_set_state. */
JSUI_API int jsui_patch_state(JsuiRuntime* rt, const char* key, const char* merge_patch_json);
/* Current JSON text of `key` (values the UI set included), or NULL. Valid
 * until the next call into the runtime. */
JSUI_API const char* jsui_get_state(JsuiRuntime* rt, const char* key);

/* JsuiConfig.on_state: JS changed a key (`HostState.set(key, value)` /
 * `HostState.patch(key, patch)`, for UI-owned data such as settings). `json`
 * is the key's full new JSON text, NULL when JS removed it. Not called for the
 * embedder's own jsui_set_state / jsui_patch_state. Runs synchronously while
 * JS is on the stack (jsui_get_state / jsui_set_state are fine inside). */
typedef void (*JsuiStateFn)(void* user, const char* key, const char* json);

/* ------------------------------------------------------- Calls
 *
 * UI -> game requests with an answer. In JS:
 *
 *     const res = await Host.call('shop.buy', { item: 'potion' }, { timeout: 5000 });
 *
 * calls JsuiConfig.on_call with a new call id (never 0, unique for the
 * runtime's lifetime). The embedder answers exactly once, right away or on
 * any later frame, with jsui_resolve_call (the promise resolves with the
 * parsed JSON) or jsui_reject_call (it rejects with an Error carrying
 * `message`). Without on_call, Host.call rejects at once.
 *
 * A call that ends unanswered (the JS timeout expired, or the JS context went
 * away: jsui_reload, a dev-mode reload, jsui_destroy) is reported to
 * on_call_cancel (optional) so the embedder can drop its work; answering it
 * afterwards returns -1 and does nothing.
 */
typedef void (*JsuiCallFn)(void* user, uint64_t call_id, const char* name, const char* args_json);
typedef void (*JsuiCallCancelFn)(void* user, uint64_t call_id);

/* Resolve call `call_id` with the JSON text `json` (NULL: null). Returns 0;
 * -1 if the call is unknown, already settled or cancelled; -2 if `json` is
 * not valid JSON (the call is then rejected with the parse error, so the
 * promise never hangs). May be called from inside on_call. */
JSUI_API int jsui_resolve_call(JsuiRuntime* rt, uint64_t call_id, const char* json);
/* Reject call `call_id`; `message` (UTF-8, NULL: "rejected") becomes the
 * Error's message. Returns 0, or -1 if the call is unknown or settled. */
JSUI_API int jsui_reject_call(JsuiRuntime* rt, uint64_t call_id, const char* message);

/* Game -> UI requests. JS registers a handler:
 *
 *     Host.handle('ui.confirm', async ({ text }) => await showDialog(text));
 *
 * and jsui_call_ui(rt, "ui.confirm", "{\"text\":\"Quit?\"}", done, user)
 * runs it. The handler may return a value or a promise. When it settles,
 * `done(user, call_id, ok, result)` runs with ok = 1 and the result as JSON
 * text, or ok = 0 and an error message: the handler threw or rejected, no
 * handler is registered for `name`, the result is not JSON-serializable, or
 * the JS context went away first. `done` runs exactly once: inside a later
 * jsui_tick() (never inside jsui_call_ui), or from jsui_reload() /
 * jsui_destroy(); it must not destroy the runtime. `done` may be NULL.
 * Returns the call id (never 0). */
typedef void (*JsuiCallResultFn)(void* user, uint64_t call_id, int ok, const char* result);
JSUI_API uint64_t jsui_call_ui(JsuiRuntime* rt, const char* name, const char* args_json, JsuiCallResultFn done,
                               void* user);

/* ------------------------------------------------------- Frame data
 *
 * Numbers the game pushes every frame (30 enemy positions for nameplates
 * and markers, minimap blips, cooldown timers): named typed arrays, no JSON.
 * Like shared state they belong to the runtime (set them any time; they
 * survive reloads) and are shared by all of its surfaces. JS sees a buffer as
 * a typed array over the runtime's own memory (no copy, no parsing):
 *
 *     // Re-render only when the number of enemies changes ...
 *     const count = useHostBufferSelector('enemies', (b) => b.length / 4, 0);
 *     // ... and let the runtime move each marker every frame, no JS at all:
 *     const x = useHostBufferValue('enemies', i * 4), y = useHostBufferValue('enemies', i * 4 + 1);
 *     <Animated.View style={{ transform: [{ translateX: x }, { translateY: y }] }} />
 *
 *     useHostBuffer('enemies')                 // Float32Array; re-renders on every change
 *     useHostBufferListener('enemies', (b) => ...)   // per change, no re-render
 *     HostBuffer.get('enemies') / subscribe(key, cb)
 *
 * Animated.hostBuffer(key, index) (useHostBufferValue) is a native Animated
 * node: the runtime writes the views that use it (transforms, opacity,
 * colors through interpolate(), ...) in the jsui_tick() or jsui_tick_native()
 * after the buffer changed. Changed keys reach JS subscribers once per
 * jsui_tick(), like shared state.
 *
 * The layout of a buffer is up to the game and the UI (e.g. x, y, hp, flags
 * per enemy). Setting the same contents again is a no-op. Treat the JS view as
 * read-only; it is valid until the buffer grows past its capacity or is
 * removed, after which it is detached (length 0): read it through the API
 * each time instead of keeping it. */
typedef enum JsuiBufferType {
    JSUI_BUFFER_F32 = 0, /* Float32Array */
    JSUI_BUFFER_F64,     /* Float64Array */
    JSUI_BUFFER_I32,     /* Int32Array */
    JSUI_BUFFER_U32,     /* Uint32Array */
    JSUI_BUFFER_I16,     /* Int16Array */
    JSUI_BUFFER_U16,     /* Uint16Array */
    JSUI_BUFFER_I8,      /* Int8Array */
    JSUI_BUFFER_U8       /* Uint8Array */
} JsuiBufferType;

/* Copies `count` elements of `type` from `data` into buffer `key`. Returns 1
 * if the contents changed, 0 if they were the same, -1 for a NULL key or an
 * invalid type. */
JSUI_API int jsui_set_buffer(JsuiRuntime* rt, const char* key, JsuiBufferType type, const void* data, size_t count);
/* Zero-copy alternative: returns the buffer's storage for `count` elements of
 * `type` (earlier contents kept up to that length, new elements 0) for the
 * game to write into, then jsui_commit_buffer(). Write and commit before the
 * next jsui_tick() or input call. The pointer is valid until the next
 * jsui_map_buffer / jsui_set_buffer / jsui_remove_buffer of that key. NULL for
 * a NULL key or an invalid type. */
JSUI_API void* jsui_map_buffer(JsuiRuntime* rt, const char* key, JsuiBufferType type, size_t count);
/* Publishes what was written through jsui_map_buffer (always counts as a
 * change). Returns 1, or -1 for an unknown key. */
JSUI_API int jsui_commit_buffer(JsuiRuntime* rt, const char* key);
/* Removes buffer `key` (JS then sees undefined). Returns 1 if it existed. */
JSUI_API int jsui_remove_buffer(JsuiRuntime* rt, const char* key);
/* The committed contents of `key` (NULL if unset); `type` and `count` may be
 * NULL. Valid until the buffer is next changed. */
JSUI_API const void* jsui_get_buffer(JsuiRuntime* rt, const char* key, JsuiBufferType* type, size_t* count);

/* ------------------------------------------------------- Host textures
 *
 * Pictures the host makes itself, shown inside the UI: a video the engine
 * decodes (Godot's VideoStreamPlayer texture, WebCodecs VideoFrames, a
 * <video> element), a 3D viewport or render target (a character preview, a
 * minimap camera), a webcam. JSUI does not decode or play anything: the host
 * names a texture, keeps it current, and the UI shows it by that name:
 *
 *     <HostTexture name="trailer" resizeMode="contain" style={{ width: 640, aspectRatio: 16 / 9 }} />
 *     const size = useHostTexture('trailer');   // { width, height } once set, else null
 *
 * HostTexture lays out and paints like Image (its intrinsic size is the
 * texture's size in px, resizeMode, borderRadius clipping, tintColor,
 * opacity, transforms). Like frame data, textures belong to the runtime
 * (set them any time; they survive reloads) and every surface can show them.
 *
 * A texture is either CPU pixels (jsui_set_texture_pixels: copied, shown by
 * every renderer, the CPU raster included) or a GPU texture of the device
 * the runtime renders with (jsui_set_texture), sampled in place, no copy:
 *
 *     kind                  handle             renders that can show it
 *     JSUI_TEXTURE_GL       GL texture name    jsui_render_gl* (same / shared GL context)
 *     JSUI_TEXTURE_WEBGPU   WGPUTexture        jsui_gpu_create_webgpu, jsui_gpu_create_dawn*
 *     JSUI_TEXTURE_VULKAN   VkImage            jsui_gpu_create_vulkan
 *     JSUI_TEXTURE_METAL    id<MTLTexture>     jsui_gpu_create_metal, jsui_metal_create
 *     JSUI_TEXTURE_D3D12    ID3D12Resource*    jsui_gpu_create_d3d12
 *
 * A render that cannot sample a texture (another API, the CPU raster for a
 * GPU texture) leaves its views empty and logs one warning per texture.
 *
 * Per frame: when the host draws new contents into the same texture (the next
 * video frame), it calls jsui_texture_updated(); the runtime then repaints
 * just the views showing it (no layout, no JS). Setting a texture with a
 * different handle, size or format also re-lays out the views whose size
 * comes from it, and tells JS (useHostTexture) in the next jsui_tick().
 *
 * GPU textures stay the host's: JSUI keeps no reference past a render (its
 * GPU work is submitted before the render returns). The host keeps a texture
 * alive while it is set and while that work may still run, and orders its own
 * writes before the render: on the same queue / GL context before the call
 * (Vulkan: the render's full barriers order it; Metal: the same queue;
 * WebGPU: queue order). Formats: 8-bit RGBA or BGRA, UNORM, holding
 * sRGB-encoded values like images do (Vulkan / Metal / WebGPU / GL format 0:
 * RGBA8 UNORM; D3D12 format 0: the resource's own, RGBA8 / BGRA8 UNORM or
 * TYPELESS). Vulkan images must be in SHADER_READ_ONLY_OPTIMAL whenever a
 * render runs (JSUI leaves them in it). D3D12 resources must be in the state
 * JsuiTexture.usage names (0: PIXEL_SHADER_RESOURCE) whenever a render runs:
 * a render that samples one in another state transitions it back before it
 * returns (in its own command list on the queue), so the host's state
 * tracking stays valid. A D3D12 context holds a reference (AddRef) to a
 * resource while it keeps it wrapped (until the end of the next frame that
 * does not show it, or jsui_gpu_purge). */
typedef enum JsuiTextureKind {
    JSUI_TEXTURE_PIXELS = 0, /* CPU pixels (jsui_set_texture_pixels) */
    JSUI_TEXTURE_GL,         /* handle: the GL texture name, (void*)(uintptr_t)name; GL_TEXTURE_2D */
    JSUI_TEXTURE_WEBGPU,     /* handle: WGPUTexture (TextureBinding usage) */
    JSUI_TEXTURE_VULKAN,     /* handle: VkImage (SAMPLED usage), on the context's device */
    JSUI_TEXTURE_METAL,      /* handle: id<MTLTexture> (ShaderRead usage), on the context's device */
    JSUI_TEXTURE_D3D12       /* handle: ID3D12Resource*, on the context's device (state: usage) */
} JsuiTextureKind;

enum {
    /* The pixels are not premultiplied by alpha (straight alpha: most image
     * files, Godot images, ImageData). Default: premultiplied. */
    JSUI_TEXTURE_UNPREMULTIPLIED = 1 << 0,
    /* Ignore alpha: every pixel is opaque (video frames). */
    JSUI_TEXTURE_OPAQUE = 1 << 1,
    /* Row 0 is the bottom of the picture (a GL render target drawn the GL
     * way). Default: row 0 is the top (uploaded images and video frames). */
    JSUI_TEXTURE_BOTTOM_LEFT_ORIGIN = 1 << 2
};

typedef struct JsuiTexture {
    int kind;             /* JsuiTextureKind (not JSUI_TEXTURE_PIXELS) */
    void* handle;         /* see JsuiTextureKind */
    /* VkFormat / MTLPixelFormat / WGPUTextureFormat / GL sized internal format
     * (GL_RGBA8, GL_BGRA8_EXT); 0: RGBA8 UNORM. */
    unsigned int format;
    int width, height;    /* px */
    int flags;            /* JSUI_TEXTURE_* */
    /* Vulkan: the VkImageUsageFlags the image was created with (0: SAMPLED |
     * TRANSFER_SRC | TRANSFER_DST). D3D12: the D3D12_RESOURCE_STATES the
     * resource is in whenever a render runs, and is left in (0:
     * PIXEL_SHADER_RESOURCE). Unused elsewhere. */
    unsigned int usage;
} JsuiTexture;

/* Sets texture `key` to a GPU texture (`texture` NULL removes it). Returns 1
 * when the texture changed, 0 when it is the same as before (same handle,
 * size, format and flags: use jsui_texture_updated for new contents), -1 for
 * a NULL key or an invalid texture (no handle, size <= 0, unknown kind). */
JSUI_API int jsui_set_texture(JsuiRuntime* rt, const char* key, const JsuiTexture* texture);
/* Sets texture `key` to a copy of `width` x `height` RGBA8 pixels (rows
 * `row_bytes` apart; 0: width * 4), sRGB-encoded, flags JSUI_TEXTURE_*.
 * Every call is new contents. Returns 1, or -1 for a NULL key or bad size. */
JSUI_API int jsui_set_texture_pixels(JsuiRuntime* rt, const char* key, const void* rgba, int width, int height,
                                     size_t row_bytes, int flags);
/* The host drew new contents into texture `key` (same handle): repaints the
 * views showing it. Returns 1, or -1 for an unknown key. */
JSUI_API int jsui_texture_updated(JsuiRuntime* rt, const char* key);
/* Removes texture `key` (its views draw nothing; JS sees null). Returns 1 if
 * it existed. */
JSUI_API int jsui_remove_texture(JsuiRuntime* rt, const char* key);

/* ------------------------------------------------------------------ Audio
 *
 * UI sounds (hover, press, focus moves, confirm / back), music and ambience.
 * The UI asks for sounds; the host plays them. In JS:
 *
 *     Sound.play(require('./ui/confirm.wav'), { bus: 'ui', volume: 0.8 });
 *     useMusic(require('./menu.ogg'), { fadeIn: 1500 });        // plays while mounted
 *     <Pressable sounds={{ hoverIn: tick, press: confirm }} />  // or <UISounds> for a whole screen
 *
 * Every request becomes a JsuiAudioCommand passed to the audio handler
 * (JsuiConfig.on_audio). A game engine routes them through its own mixer
 * (Godot: AudioStreamPlayers on its audio buses), so UI sounds follow the
 * game's volume settings and effects. Without a handler the runtime plays
 * them itself: the native library's built-in backend (miniaudio, built when
 * JSUI_AUDIO is ON, the default on desktop platforms) and, in the browser,
 * Web Audio in web/jsui-web.js. JsuiConfig.audio_disabled = 1 turns sound
 * off (headless tools, tests). The built-in backend opens the output device
 * on the first PLAY / PRELOAD; JSUI_AUDIO_BACKEND=null (environment) makes
 * it play to a null device (CI without sound hardware). See docs/AUDIO.md.
 *
 * Sounds are URLs resolved against the base URL (the bundler emits imported
 * .wav / .ogg / .mp3 / .flac files next to the bundle). A handler loads them
 * as it likes; jsui_fetch() reads them through the runtime's own loader (dev
 * server, file system, a custom JsuiLoadFn).
 *
 * Voices: every PLAY carries a new voice id (never 0). The handler reports
 * the end of each voice exactly once with jsui_audio_voice_ended(): it
 * played to the end, it was stopped (after the fade out), or it could not
 * load / play. A looping voice ends only when stopped. Commands for a voice
 * that has ended are ignored by the handler.
 *
 * Buses are named groups of voices ("ui", "sfx", "music", "voice", ... any
 * name): the UI's own volume / mute per group, multiplied with each voice's
 * volume. Handlers that map buses to an engine's buses keep the engine's
 * bus volumes separate (the game's settings stay the game's).
 *
 * Volumes are linear gains (1: unchanged, 0: silent). pitch is the playback
 * rate (1: normal; changes pitch and speed together). pan: -1 left .. 1
 * right. fade_ms ramps volume changes (PLAY: fade in from silence; STOP:
 * fade out, then end; SET / BUS: ramp to the new value).
 */
typedef enum JsuiAudioOp {
    JSUI_AUDIO_PRELOAD = 0,  /* sound: load / decode ahead so a later PLAY starts at once. Answer with
                                jsui_audio_sound_loaded() (now or later). */
    JSUI_AUDIO_UNLOAD = 1,   /* sound: no longer needed (voices playing it may finish) */
    JSUI_AUDIO_PLAY = 2,     /* voice, sound, bus, volume, pitch, pan, loop, fade_ms (fade in), offset_s */
    JSUI_AUDIO_STOP = 3,     /* voice, fade_ms (fade out, then end) */
    JSUI_AUDIO_PAUSE = 4,    /* voice */
    JSUI_AUDIO_RESUME = 5,   /* voice */
    JSUI_AUDIO_SET = 6,      /* voice: volume, pitch, pan (ramped over fade_ms) */
    JSUI_AUDIO_BUS = 7,      /* bus: volume (ramped over fade_ms), muted. Buses start at volume 1, unmuted. */
    JSUI_AUDIO_STOP_ALL = 8  /* bus (NULL: every voice), fade_ms. Sent with bus NULL and fade 0 when the JS
                                context goes away (jsui_reload, dev reloads, jsui_destroy). */
} JsuiAudioOp;

typedef struct JsuiAudioCommand {
    JsuiAudioOp op;
    uint64_t voice;     /* PLAY, STOP, PAUSE, RESUME, SET */
    const char* sound;  /* PRELOAD, UNLOAD, PLAY: absolute URL */
    const char* bus;    /* PLAY, BUS: never NULL; STOP_ALL: NULL for every bus */
    float volume;       /* PLAY, SET, BUS */
    float pitch;        /* PLAY, SET */
    float pan;          /* PLAY, SET */
    float fade_ms;      /* PLAY, STOP, SET, BUS, STOP_ALL */
    double offset_s;    /* PLAY: start this far into the sound */
    int loop;           /* PLAY */
    int muted;          /* BUS */
} JsuiAudioCommand;

/* Called synchronously (usually while JS runs) on the runtime's thread. The
 * command and its strings are valid only during the call. */
typedef void (*JsuiAudioFn)(void* user, const JsuiAudioCommand* cmd);

enum {
    JSUI_AUDIO_FINISHED = 0, /* played to the end */
    JSUI_AUDIO_STOPPED = 1,  /* STOP / STOP_ALL */
    JSUI_AUDIO_FAILED = -1   /* the sound could not be loaded, decoded or played */
};
/* Voice `voice` ended (JSUI_AUDIO_FINISHED / _STOPPED / _FAILED). Call it
 * exactly once per PLAY, also from inside the handler; JS hears about it in
 * the next jsui_tick(). Unknown or already ended voices are ignored. */
JSUI_API void jsui_audio_voice_ended(JsuiRuntime* rt, uint64_t voice, int status);
/* Sound `sound` (the URL from the command) finished loading: ok 1 with its
 * duration in seconds (< 0: unknown), or ok 0. Resolves JS's
 * Sound.preload() promises for it. */
JSUI_API void jsui_audio_sound_loaded(JsuiRuntime* rt, const char* sound, int ok, double duration_s);

/* Reads `url` (relative URLs resolve against the base URL) through the
 * runtime's loader, the same way the bundle and images are read. `done` runs
 * once on the runtime's thread inside a later jsui_tick() with an HTTP-like
 * status (200 ok, 404 missing, 0 network error) and the bytes (valid only
 * during the call). It never runs if the runtime is destroyed first. */
typedef void (*JsuiFetchFn)(void* user, int status, const char* content_type, const void* data, size_t size);
JSUI_API void jsui_fetch(JsuiRuntime* rt, const char* url, JsuiFetchFn done, void* user);

/* ---------------------------------------------------------------- Config */

typedef struct JsuiConfig {
    int width;              /* viewport size in logical px */
    int height;
    float pixel_ratio;      /* device pixels per logical px */
    const char* base_url;   /* resolves relative URLs; defaults to the bundle URL */
    int dev_mode;           /* 1: poll the dev server and hot-update / reload on change, show error overlay */

    JsuiLogFn log;          /* NULL: print to stderr / console */
    void* log_user;
    JsuiLoadFn load;        /* NULL: built-in loader */
    void* load_user;
    JsuiMessageFn on_message;
    void* message_user;
    JsuiStateFn on_state;             /* JS changed a shared-state key (optional) */
    void* state_user;
    JsuiCallFn on_call;               /* Host.call() from JS (optional) */
    JsuiCallCancelFn on_call_cancel;  /* a call ended unanswered (optional) */
    void* call_user;                  /* passed to on_call / on_call_cancel */

    /* JS engine limits (0: default). The watchdog interrupts any single
     * native->JS call (an event, a timer, a frame callback) that runs longer
     * than js_timeout_ms and reports it as an error; negative disables it. */
    int js_memory_limit_mb; /* default 256 */
    int js_stack_size_kb;   /* default 1024 (512 on Windows, whose threads get 1 MB stacks) */
    int js_timeout_ms;      /* default 5000 */

    JsuiAudioFn on_audio;   /* plays the UI's sounds (see "Audio"); NULL: the built-in backend */
    void* audio_user;
    int audio_disabled;     /* 1: no sound at all (every voice fails, preloads fail) */
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
/* Precompiled bundles. `jsui build` writes the bundle as QuickJS bytecode in
 * a small container, which both load functions accept as is: no parsing at
 * startup and less memory. Bytecode only runs on the QuickJS build that
 * compiled it (jsui_bytecode_key()); any other runtime runs the source
 * instead and logs a warning: the source inside the container (`jsui build`'s
 * default "combo"), else the separate source file it names ("split", loaded
 * through the loader relative to the bundle's name / URL).
 * jsui_compile_bundle compiles `source` (a global script, e.g. a `jsui build
 * --format source` bundle) into such a container; `keep_source` = 0 leaves
 * the source out, `source_url` (may be NULL) names the separate source file.
 * Returns 0 and the container in *out_data (free with jsui_free()), or -1
 * with the syntax error in *out_error (jsui_free()) when out_error is not
 * NULL. Needs no runtime. */
JSUI_API int jsui_compile_bundle(const char* source, size_t size, const char* name, int keep_source,
                                 const char* source_url, void** out_data, size_t* out_size, char** out_error);
/* The QuickJS build's bytecode identity (version + JSUI patches). */
JSUI_API const char* jsui_bytecode_key(void);
/* Tear down the JS context and reload the last bundle (dev mode does this automatically). */
JSUI_API void jsui_reload(JsuiRuntime* rt);
/* For hosts that watch the dev server themselves: it has a new build (or a
 * build error). Bundles from `jsui dev` hot-update in place (React Fast
 * Refresh: components keep their state) and fall back to a reload; other
 * bundles reload. Dev mode with an http:// bundle URL does this on its own. */
JSUI_API void jsui_dev_update(JsuiRuntime* rt);
/* Dev mode with an http:// bundle URL also joins the dev server's shared
 * session (`jsui dev`): coding agents attached to the server
 * (`jsui run --attach`) can then drive this runtime (input, state, calls,
 * screenshots) and see what the person does in it. `name` is shown there
 * (e.g. "AW3 main menu"; default: the platform); `enabled` 0 keeps this
 * runtime out of the session. Call before loading the bundle. */
JSUI_API void jsui_set_dev_session(JsuiRuntime* rt, const char* name, int enabled);

/* -------------------------------------------------------------- Fonts
 *
 * Fonts are runtime-wide (every surface). Inter is built in and is the
 * default family. App code loads its own with Fonts.load / useFonts (from the
 * bundle, a URL or bytes); hosts can register fonts they already have.
 *
 * Register the faces in `data` (TTF, OTF, a TTC / OTC collection: every face,
 * WOFF or WOFF2; copied) under `family`, like a CSS @font-face rule:
 *   family  NULL or "" = the family name inside the file
 *   weight  1..1000 overrides the file's weight; <= 0 = from the file. A
 *           variable font with a 'wght' axis and weight <= 0 is registered
 *           once per 100 step of its weight range, so fontWeight picks the
 *           instance; with a weight it is pinned to that weight.
 *   italic  0/1 overrides the file's slant; < 0 = from the file
 * Registering the same family and style again replaces that face. Text on
 * every surface is laid out again. Returns 0, or -1 if the data is not a font. */
JSUI_API int jsui_register_font(JsuiRuntime* rt, const char* family, const void* data, size_t size,
                                int weight, int italic);
/* Remove every face of `family` (case-insensitive). 0, or -1 if unknown. */
JSUI_API int jsui_unregister_font(JsuiRuntime* rt, const char* family);
/* Families tried in order for characters the styled font lacks (emoji, CJK,
 * symbols), comma separated ("Noto Sans JP, Noto Color Emoji"); NULL or ""
 * clears. Per-glyph fallback then tries every other registered font, then
 * the system fonts where the platform has them (not on the web). */
JSUI_API void jsui_set_font_fallbacks(JsuiRuntime* rt, const char* families);
/* The family used when a style names none (default "Inter"). */
JSUI_API void jsui_set_default_font_family(JsuiRuntime* rt, const char* family);

/* -------------------------------------------------------------- Surfaces
 *
 * One runtime (one bundle, one JS context, shared state, fonts and image
 * cache) can show several independent root views, like React Native's
 * RCTRootView / ReactRootView: the bundle registers components with
 * AppRegistry.registerComponent('Nameplate', () => Nameplate) and the host
 * creates a surface per place it wants one (a HUD, a nameplate over each
 * character, a widget inside an existing UI), each with its own initial
 * props, viewport, input, focus, cursor and render target.
 *
 * A surface handle is a JsuiRuntime*: the per-surface calls take it in place
 * of the runtime handle (jsui_set_viewport, jsui_needs_redraw, every
 * jsui_render_* call, every jsui_send_* / input call, jsui_get_cursor).
 * Runtime-wide calls (jsui_tick, bundles, state, calls, fonts, reload,
 * jsui_wants_frame) act on the shared runtime whichever handle is passed:
 * tick the runtime once per frame, then render each surface that
 * jsui_needs_redraw. The runtime handle itself is the main surface (root tag
 * 1), which runs the first registered component unless jsui_surface_run
 * picks one.
 *
 * Surface roots render with a transparent background. A width or height
 * <= 0 sizes that axis to the component's content: read the laid-out size
 * with jsui_surface_get_size after a tick and size the render target to it
 * (times the pixel ratio).
 *
 * In JS a surface's app receives its props, and RootTagContext /
 * useWindowDimensions() give its root tag and size; Dimensions.get('window')
 * stays the main surface's. Surfaces survive jsui_reload (their apps start
 * again with the new bundle). */

/* Creates a surface showing the component registered as `app_key` with
 * `initial_props_json` (a JSON object; NULL: {}). Returns NULL (and logs) on
 * a missing key or invalid props. The component mounts on the next
 * jsui_tick, in one batch with the other surfaces started or updated since
 * the last tick. A key the bundle registers later (after an await, say)
 * starts the surface when it is registered. */
JSUI_API JsuiRuntime* jsui_surface_create(JsuiRuntime* rt, const char* app_key, const char* initial_props_json,
                                          int width, int height, float pixel_ratio);
/* Unmounts the surface's component and frees the surface; the handle is
 * invalid afterwards. Not for the runtime handle (jsui_destroy frees the
 * runtime, its main surface and every surface handle still alive). */
JSUI_API void jsui_surface_destroy(JsuiRuntime* surface);
/* Runs `app_key` (NULL: the surface's current one) on the surface with new
 * props: the component re-renders with them, keeping its state when the key
 * is unchanged (React Native's appProperties). Applied on the next
 * jsui_tick, batched with every other surface's update (one React commit);
 * the last call before the tick wins, and the same key and props as the
 * surface already has cost nothing, so a host may push props every frame.
 * On the runtime handle this picks the main surface's component. Returns 0,
 * or -1 for invalid props. */
JSUI_API int jsui_surface_run(JsuiRuntime* surface, const char* app_key, const char* props_json);
/* jsui_surface_run(surface, NULL, props_json). */
JSUI_API int jsui_surface_set_props(JsuiRuntime* surface, const char* props_json);
/* The surface's React Native root tag (1 for the main surface). */
JSUI_API int jsui_surface_tag(JsuiRuntime* surface);
/* The runtime handle a surface belongs to (itself for the runtime handle). */
JSUI_API JsuiRuntime* jsui_surface_runtime(JsuiRuntime* surface);
/* The surface's laid-out size in logical px (auto-sized axes report their
 * content's size; runs layout if needed). Returns 0. */
JSUI_API int jsui_surface_get_size(JsuiRuntime* surface, float* width, float* height);

/* ------------------------------------------------------------ Frame loop */

/* The viewport of the handle's surface (see Surfaces for sizes <= 0). */
JSUI_API void jsui_set_viewport(JsuiRuntime* rt, int width, int height, float pixel_ratio);
/* Advance time to `now_ms` (monotonic). Runs due timers, resolves loaded
 * resources, runs requestAnimationFrame callbacks, microtasks and layout. */
JSUI_API void jsui_tick(JsuiRuntime* rt, double now_ms);
/* Advances only the work that runs natively on the runtime clock, without
 * running any JS: Animated animations started with useNativeDriver: true
 * (and the styles they drive), LayoutAnimations and layout. Timers,
 * requestAnimationFrame, input events and animation end callbacks wait for
 * the next jsui_tick(). For hosts that skip JS on a frame (a frame budget
 * already spent, a hitch while loading) but keep native animations smooth.
 * now_ms is on the same clock as jsui_tick(). */
JSUI_API void jsui_tick_native(JsuiRuntime* rt, double now_ms);
/* How much wall time (ms) the JS of one jsui_tick() may take before
 * React's scheduler yields to the next frame (default 5). Concurrent
 * renders (startTransition, useDeferredValue) are then time-sliced: each
 * tick renders part of the new tree while the current UI stays live, and
 * the commit, passive effects and zero-delay timers queued past the budget
 * follow in later ticks. Synchronous updates (a plain setState in an event
 * handler) still render in one go. <= 0 removes the limit (a transition
 * renders in the tick it starts). Applies to every surface of the runtime;
 * performance.now() stays the runtime clock either way. */
JSUI_API void jsui_set_js_frame_budget(JsuiRuntime* rt, double ms);
JSUI_API double jsui_get_js_frame_budget(JsuiRuntime* rt);
/* Number of resource loads (bundle, fetch, images, fonts) still in flight.
 * Headless hosts keep ticking until this reaches 0 before capturing. */
JSUI_API int jsui_pending_loads(JsuiRuntime* rt);
/* Non-zero when the last tick changed what the handle's surface would paint. */
JSUI_API int jsui_needs_redraw(JsuiRuntime* rt);
/* Non-zero while something is in motion and the next frame should be ticked
 * soon even without input: requestAnimationFrame callbacks, due timers,
 * scroll animations (flings, smooth scrolling, snapping), scroll indicator
 * fades, held gamepad sticks / D-pad, native-driven Animated animations,
 * LayoutAnimations, surfaces started / given new props / resized since the
 * last tick. Hosts that sleep when idle keep calling
 * jsui_tick at display rate while this is set. */
JSUI_API int jsui_wants_frame(JsuiRuntime* rt);
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

/* ------------------------------------------------------------- Profiling
 *
 * Where a runtime's frames spend their time, e.g. for an in-game stats
 * overlay or to find what makes a frame hitch. The work counters are always
 * on (a few increments per operation). With profiling on, every tick, input
 * call and render also accumulates wall time per phase (two clock reads per
 * phase change). Phases nest and are exclusive: a phase entered from another
 * pauses it (a React commit's style update is js -> commit -> style), so the
 * *_us phase fields add up to the time since the reset.
 *
 * A runtime's surfaces (jsui_surface_create) share its totals and switch:
 * a surface handle reports and toggles the same numbers as the runtime
 * handle, which cover every surface's commits, style, layout, input and
 * renders.
 *
 *     jsui_set_profiling(rt, 1);
 *     ... frames ...
 *     JsuiPerf p;
 *     jsui_get_perf(rt, &p, 1);  // since the last reset, and reset
 */
typedef struct JsuiPerf {
    /* Exclusive time per phase in microseconds (0 while profiling is off). */
    double js_us;         /* QuickJS: timers, requestAnimationFrame, events, microtasks */
    double commit_us;     /* the native side of React's tree and prop mutations */
    double animated_us;   /* native-driven Animated */
    double style_us;      /* style resolution */
    double layout_us;     /* Yoga layout and text measurement */
    double input_us;      /* hit testing and event routing (JS handlers count as js_us) */
    double paint_us;      /* renders: recording the frame (the CPU raster also rasterises it) */
    double other_us;      /* everything else, including the time between JSUI calls */
    double gpu_submit_us; /* Graphite renders (jsui_render_gpu_target / _webgpu): submitting the
                             recorded frame, which includes compiling new GPU pipelines and
                             uploading textures */
    /* Always measured. */
    double image_decode_us;               /* decoding images (also inside the phase that needed them) */
    unsigned long long image_decodes;
    unsigned long long pipeline_compiles; /* GPU pipelines compiled on demand by this runtime's renders:
                                             each is a possible hitch (see the pipeline cache) */
    unsigned long long paragraph_builds;  /* text shaping */
    unsigned long long text_measures;     /* text measured for layout */
    unsigned long long layout_passes;
    unsigned long long bridge_calls;      /* React -> native mutation calls */
} JsuiPerf;

/* Turns the per-phase timing on (non-zero) or off. Off by default. */
JSUI_API void jsui_set_profiling(JsuiRuntime* rt, int on);
/* The totals since the runtime was created or last reset; `reset` non-zero
 * starts new totals. */
JSUI_API void jsui_get_perf(JsuiRuntime* rt, JsuiPerf* out, int reset);

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
 * - The target's previous contents are not assumed to be preserved. When
 *   little changed, each runtime's last frame is kept in a texture of the
 *   JsuiGpuContext (one per runtime), only the damage is repainted into it
 *   and it is drawn into the target with one draw; when most of the frame
 *   changed, the frame is painted straight into the target. JSUI_GL_NO_RETAIN
 *   and MSAA targets always paint the whole frame into the target.
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
    JSUI_GL_NO_CLEAR = 1 << 1,
    /* Paint the whole frame straight into the target every render. Default:
     * the runtime keeps its last frame in a texture of the JsuiGpuContext,
     * repaints only what changed into it and draws it into the target with
     * one draw (so the target's previous contents need not be kept). */
    JSUI_GL_NO_RETAIN = 1 << 2
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

/* ------------------------------------------------------------ GPU (WebGPU)
 *
 * Optional GPU backend: Skia Graphite draws the frame into a WebGPU texture
 * of the embedder's device (the browser's WebGPU in the WASM build, or Dawn
 * native, e.g. a game engine's WebGPU renderer). Available when the library
 * was built with JSUI_WEBGPU (scripts/build-wasm.sh GPU=1); otherwise
 * jsui_gpu_create_webgpu() returns NULL. The handles are webgpu.h objects
 * passed as void* so this header does not depend on webgpu.h.
 *
 *     // once:
 *     JsuiGpuContext* gpu = jsui_gpu_create_webgpu(instance, device, NULL);
 *     // per frame (e.g. into the swapchain / canvas texture):
 *     jsui_tick(rt, now_ms);
 *     if (jsui_needs_redraw(rt))
 *         jsui_render_webgpu(rt, gpu, texture, width, height, 0);
 *     jsui_gpu_destroy(gpu);
 *
 * - The context keeps its own references to instance / device / queue; the
 *   target texture is only used during the call (Skia's recording is
 *   submitted to the queue before it returns).
 * - The target needs WGPUTextureUsage_RenderAttachment (TextureBinding or
 *   CopySrc as well lets Skia read back the destination for some blend
 *   modes) and an 8-bit RGBA / BGRA (optionally sRGB) or RGBA16Float format.
 * - Incremental repaint: by default a frame where little changed repaints
 *   only the damage bounds into a texture owned by the context, which is then
 *   copied to the target with one draw; a frame where most of it changed is
 *   painted straight into the target. Either way the target's previous
 *   contents do not matter (canvas / swapchain textures start cleared), and a
 *   frame where only a spinner moved repaints just the spinner.
 *   JSUI_WEBGPU_NO_RETAIN always paints the whole frame straight into the
 *   target (one texture less of memory).
 * - Device loss: renders fail (-1) once the device is lost. Call
 *   jsui_gpu_context_lost() / jsui_gpu_destroy() and create a new context on
 *   a new device; runtimes keep no GPU objects, so nothing else changes.
 * - Not thread safe: use a context from one thread at a time. On Dawn native
 *   the context calls wgpuInstanceProcessEvents on `instance` while it waits
 *   for GPU work (pass the instance the device came from).
 */
enum {
    /* Draw over the target's current contents (premultiplied source-over).
     * Default: replace them (the UI's transparent pixels stay transparent). */
    JSUI_WEBGPU_NO_CLEAR = 1 << 1,
    /* Paint straight into the target (full repaint every frame) instead of
     * through the context's retained frame texture. */
    JSUI_WEBGPU_NO_RETAIN = 1 << 2,
    /* Render opacity layers offscreen every time they are painted instead of
     * compositing their cached textures (see "Composited layers" in
     * docs/ARCHITECTURE.md; for comparisons). */
    JSUI_WEBGPU_NO_LAYER_CACHE = 1 << 3,
    /* Paint subtrees that keep being repainted unchanged draw by draw
     * instead of from cached textures of them (see "Raster cache" in
     * docs/ARCHITECTURE.md; for comparisons). */
    JSUI_WEBGPU_NO_RASTER_CACHE = 1 << 4,
    /* Repaint damage over a cleared target instead of over a cached
     * texture of the static background (see "Static backdrop" in
     * docs/ARCHITECTURE.md; for comparisons). */
    JSUI_WEBGPU_NO_BACKDROP = 1 << 5
};

typedef struct JsuiWebGPUTarget {
    void* texture;   /* WGPUTexture to draw into */
    int width, height; /* device px; 0: the texture's size */
    int flags;       /* JSUI_WEBGPU_* */
} JsuiWebGPUTarget;

/* Creates a Graphite context on `device` (WGPUDevice, required). `instance`
 * (WGPUInstance) may be NULL in the browser; Dawn native needs it. `queue`
 * NULL uses the device's queue. Returns NULL when the library was built
 * without JSUI_WEBGPU or Graphite rejects the device. */
JSUI_API JsuiGpuContext* jsui_gpu_create_webgpu(void* instance, void* device, void* queue);
/* Paints the current frame into `texture` (WGPUTexture). Like
 * jsui_render_rgba it lays out first and clears needs_redraw. Returns 0 on
 * success, -1 without a usable context / target (or after device loss). */
JSUI_API int jsui_render_webgpu(JsuiRuntime* rt, JsuiGpuContext* gpu, void* texture, int width, int height, int flags);
JSUI_API int jsui_render_webgpu_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiWebGPUTarget* target);

/* ------------------------------------------- GPU (Vulkan / Metal, Graphite)
 *
 * Skia Graphite draws the frame into a texture of an engine's own Vulkan
 * device (VkImage) or Metal device (id<MTLTexture>), e.g. a texture created
 * by Godot's RenderingDevice. Available when the library was built with
 * JSUI_VULKAN / JSUI_GRAPHITE_METAL (a Skia from scripts/build-skia-desktop.sh);
 * otherwise the create functions return NULL and the host keeps using
 * jsui_render_rgba(). Handles are passed as void* so this header does not
 * depend on vulkan.h / Metal.
 *
 *     JsuiVulkanDevice d = {0};
 *     d.instance = instance; d.physical_device = phys; d.device = device;
 *     d.queue = queue; d.queue_family = family;
 *     JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
 *     // per frame, on the thread that owns `queue`:
 *     jsui_tick(rt, now_ms);
 *     if (jsui_needs_redraw(rt)) {
 *         JsuiGpuTarget t = {0};
 *         t.texture = image; t.width = w; t.height = h;
 *         t.layout = t.final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 *         t.flags = JSUI_GPU_INCREMENTAL;   // the image still holds our last frame
 *         JsuiRect dirty;
 *         jsui_render_gpu_target(rt, gpu, &t, &dirty);
 *     }
 *     jsui_gpu_destroy(gpu);
 *
 * Synchronisation (Vulkan): every render submits its work to `queue` before
 * it returns: a full pipeline barrier (all commands, all memory), Skia's
 * command buffer, which leaves the image in `final_layout`, and another full
 * barrier, so work the host submits to the same queue afterwards sees the
 * finished image, and the render waits for host work submitted before it.
 * The host must not use the queue from another thread during the call
 * (Vulkan queues need external synchronisation). The image must be in
 * `layout` when the call is made (UNDEFINED discards it: full repaint).
 * Metal: the work is committed to `queue`; command buffers the host commits
 * to the same queue afterwards are ordered after it (tracked resources).
 *
 * Incremental repaint: with JSUI_GPU_INCREMENTAL the target is retained (the
 * host keeps the image between frames and nothing else draws into it), so
 * only the damage bounds are repainted; `dirty` receives the repainted rect
 * (empty when nothing changed), e.g. to copy just that region elsewhere.
 * Without it the whole target is repainted. Like jsui_render_rgba the render
 * lays out first and clears needs_redraw.
 *
 * One context may serve any number of runtimes; use it from one thread.
 */
enum {
    /* Draw over the target's current contents (premultiplied source-over)
     * on full repaints. Default: replace them. */
    JSUI_GPU_NO_CLEAR = 1 << 1,
    /* The target still holds the frame this runtime last rendered into it:
     * repaint only what changed. */
    JSUI_GPU_INCREMENTAL = 1 << 3
};

typedef struct JsuiVulkanDevice {
    void* instance;          /* VkInstance */
    void* physical_device;   /* VkPhysicalDevice */
    void* device;            /* VkDevice */
    void* queue;             /* VkQueue the work is submitted to */
    unsigned int queue_family; /* its queue family index (needs graphics) */
    /* VK_MAKE_API_VERSION the instance / device were created for (Skia uses
     * nothing newer); 0: 1.1 (the minimum, and the safe choice when unknown). */
    unsigned int api_version;
    /* PFN_vkGetInstanceProcAddr; NULL: from the Vulkan loader (libvulkan.so.1,
     * vulkan-1.dll, libvulkan.1.dylib / the process). */
    void* get_instance_proc_addr;
    /* Optional: extensions the instance / device were created with, and the
     * VkPhysicalDeviceFeatures2 chain they were created with. NULL / 0: none
     * assumed (always valid; Skia then uses core Vulkan 1.1 only). */
    const char* const* instance_extensions;
    int instance_extension_count;
    const char* const* device_extensions;
    int device_extension_count;
    const void* device_features2;
} JsuiVulkanDevice;

typedef struct JsuiGpuTarget {
    void* texture;           /* VkImage / id<MTLTexture> / WGPUTexture / ID3D12Resource* / ID3D11Texture2D* */
    /* VkFormat / MTLPixelFormat; 0: 8-bit RGBA unorm (VK_FORMAT_R8G8B8A8_UNORM
     * / MTLPixelFormatRGBA8Unorm). Also BGRA8 and the sRGB variants. */
    unsigned int format;
    int width, height;       /* device px (the texture's size) */
    int flags;               /* JSUI_GPU_* */
    /* Vulkan: the VkImageUsageFlags the image was created with (0: colour
     * attachment + input attachment + sampled + transfer src / dst; Skia
     * Graphite needs at least COLOR_ATTACHMENT and INPUT_ATTACHMENT), the
     * VkImageLayout it is in
     * on entry and the one to leave it in (0 = UNDEFINED on entry discards
     * the contents; final 0: COLOR_ATTACHMENT_OPTIMAL). Unused on Metal. */
    unsigned int usage;
    int layout;
    int final_layout;
} JsuiGpuTarget;

/* NULL when the library was built without JSUI_VULKAN or Skia rejects the
 * device (it needs Vulkan 1.1 and a graphics queue). */
JSUI_API JsuiGpuContext* jsui_gpu_create_vulkan(const JsuiVulkanDevice* device);
/* `device`: id<MTLDevice>, `queue`: id<MTLCommandQueue> of that device (both
 * retained). NULL without JSUI_GRAPHITE_METAL. (iOS hosts that want Ganesh
 * use jsui_metal_create below.) */
JSUI_API JsuiGpuContext* jsui_gpu_create_metal(void* device, void* queue);
/* Graphite on a native Dawn device the host owns (e.g. a game engine's WebGPU
 * renderer): `instance` (WGPUInstance, required: Graphite processes its events
 * while it waits for GPU work), `device` (WGPUDevice) and `queue` (WGPUQueue;
 * NULL: the device's queue). With this context JsuiGpuTarget.texture is a
 * WGPUTexture with RenderAttachment usage (plus TextureBinding or CopySrc
 * for blends that read the destination) and `format` a WGPUTextureFormat
 * (0: the texture's own; RGBA8 / BGRA8, unorm or sRGB). Work is submitted to
 * `queue` before the call returns; Dawn orders it with the host's
 * submissions.
 *
 * The library links no Dawn: every wgpu* function it (and Skia) calls goes
 * through a table of the host's entry points, filled when the context is
 * created, all or nothing. The host's Dawn must be the revision
 * jsui_dawn_revision() names (webgpu.h's ABI changes between revisions).
 *
 * jsui_gpu_create_dawn_procs: the table is resolved by name through
 * `wgpu_get_proc_address`, the host's Dawn `wgpuGetProcAddress`
 * (WGPUProc (*)(WGPUStringView)); the host need not export any symbol.
 * jsui_gpu_create_dawn: the same through the process's exported wgpu*
 * symbols (dlsym; unavailable on Windows).
 *
 * NULL when the library was built without JSUI_GRAPHITE_DAWN, an argument
 * is NULL, any entry point does not resolve (the table is then left as it
 * was) or Skia rejects the device. The table is process-wide: every Dawn
 * context of a process must use the same host Dawn. */
JSUI_API JsuiGpuContext* jsui_gpu_create_dawn_procs(void* wgpu_get_proc_address, void* instance, void* device,
                                                    void* queue);
JSUI_API JsuiGpuContext* jsui_gpu_create_dawn(void* instance, void* device, void* queue);
/* The Dawn revision (commit hash, e.g. "ada567700e13...") whose headers the
 * library was built against, i.e. the Dawn a host must provide; NULL when
 * built without JSUI_GRAPHITE_DAWN. */
JSUI_API const char* jsui_dawn_revision(void);
/* Paints the current frame into `target`. `dirty` (optional) receives the
 * repainted rect. Returns 0 on success, -1 on failure (device lost, bad
 * target, wrong kind of context). */
JSUI_API int jsui_render_gpu_target(JsuiRuntime* rt, JsuiGpuContext* gpu, const JsuiGpuTarget* target, JsuiRect* dirty);
/* Shorthand: an R8G8B8A8_UNORM VkImage with the default usage (colour +
 * input attachment, sampled, transfer src / dst) that is in
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL before and after the call. */
JSUI_API int jsui_render_vulkan(JsuiRuntime* rt, JsuiGpuContext* gpu, void* image, int width, int height, int flags);

/* ------------------------------------- GPU (Direct3D 12 / 11, Windows; Ganesh)
 *
 * Skia Graphite has no Direct3D backend, so on Direct3D hosts (Unreal and
 * Godot on D3D12, Unity on D3D11 / D3D12) the frame is drawn by Skia Ganesh
 * on Direct3D 12. Available when the library was built with JSUI_D3D (a
 * Windows Skia from scripts/build-skia-desktop.sh, skia_use_direct3d);
 * otherwise the create functions return NULL. Both contexts render with
 * jsui_render_gpu_target() and a JsuiGpuTarget whose fields mean:
 *
 *     texture       ID3D12Resource* (D3D12) / ID3D11Texture2D* (D3D11), a 2D
 *                   texture of the context's device, one mip, one sample
 *     format        DXGI_FORMAT; 0: the texture's own. R8G8B8A8 / B8G8R8A8,
 *                   UNORM or UNORM_SRGB (or the TYPELESS of those: then
 *                   drawn as UNORM). D3D12: no B8G8R8A8_UNORM_SRGB (Skia
 *                   Ganesh has no such format; use B8G8R8A8_TYPELESS);
 *                   R8G8B8A8_UNORM_SRGB blends in linear light, as on GL /
 *                   Vulkan. D3D11: the frame is drawn as UNORM and copied
 *                   bit for bit, so an sRGB texture gets sRGB-encoded
 *                   values blended like the CPU raster.
 *     layout        D3D12: the D3D12_RESOURCE_STATES the resource is in when
 *                   the call is made (0 = COMMON)
 *     final_layout  D3D12: the state to leave it in (e.g. 0x80
 *                   PIXEL_SHADER_RESOURCE for a texture the host samples next)
 *     usage         unused
 *
 * D3D12 (jsui_gpu_create_d3d12): Ganesh on the host's ID3D12Device and
 * ID3D12CommandQueue (a DIRECT queue; both AddRef'd). The resource needs
 * D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET. Every render executes its
 * command lists on `queue` before it returns, so it is ordered after what
 * the host executed on the queue before and before what it executes after
 * (queue order; the resource is transitioned from `layout` and back to
 * `final_layout` within the render's own command lists). The host must not
 * use the queue from another thread during the call. With
 * JSUI_GPU_INCREMENTAL only the damage is repainted into the resource;
 * without it small damage is repainted into a frame the context keeps per
 * runtime and drawn into the resource whole (`dirty`: the whole target).
 *
 * D3D11 (jsui_gpu_create_d3d11): Ganesh draws on a Direct3D 12 device the
 * context creates on the same adapter as the host's ID3D11Device (which
 * must be an ID3D11Device5, Windows 10 1703+). The frame is drawn into a
 * shared texture the context owns (one per runtime and size) and copied
 * into the host's texture with the device's immediate context, behind a
 * shared fence: the render waits on the D3D11 side for the D3D12 work and
 * the copy is queued on the immediate context like a host CopyResource
 * (call it on the thread that owns the immediate context). The host's
 * texture needs no shared or bind flags (a plain RenderTexture works;
 * DEFAULT usage, one sample). The shared texture keeps the runtime's last
 * frame, so only the damage is ever repainted; with JSUI_GPU_INCREMENTAL
 * only the damage is copied too (`dirty`: the copied rect), else the whole
 * frame. JSUI_GPU_NO_CLEAR copies the host's texture into the shared one
 * first and draws the whole frame over it.
 *
 * Device removal: a render returns -1 once the device is removed
 * (GetDeviceRemovedReason); destroy the context and create a new one on
 * the new device.
 *
 * Host textures (jsui_set_texture): JSUI_TEXTURE_D3D12 (ID3D12Resource of
 * the context's device, in the state JsuiTexture.usage names, 0:
 * PIXEL_SHADER_RESOURCE, whenever a render runs; JSUI returns it to that
 * state) on D3D12 contexts. D3D11 contexts cannot sample host textures yet.
 */
/* `device`: ID3D12Device*, `queue`: its ID3D12CommandQueue* (DIRECT). NULL
 * without JSUI_D3D, on a NULL argument, or when Skia rejects the device. */
JSUI_API JsuiGpuContext* jsui_gpu_create_d3d12(void* device, void* queue);
/* `device`: ID3D11Device* (an ID3D11Device5). NULL without JSUI_D3D, when the
 * device is not an ID3D11Device5 or no D3D12 device can be created on its
 * adapter. */
JSUI_API JsuiGpuContext* jsui_gpu_create_d3d11(void* device);

/* ---------------------------------------------- GPU pipeline cache (Graphite)
 *
 * Skia Graphite (every context above but GL: Vulkan, Metal, Dawn and
 * jsui_gpu_create_webgpu) creates a GPU pipeline (compiled shaders + render
 * state) the first time a kind of draw is needed, inside that frame: a list's
 * first scroll over images with rounded corners or a panel's first entrance
 * can stall its frame for tens of milliseconds on a cold driver. The
 * pipeline cache records every pipeline a context creates and has the next
 * session create them again up front, off the frame:
 *
 *     JsuiGpuContext* gpu = jsui_gpu_create_vulkan(&d);
 *     jsui_gpu_precompile_pipelines(gpu, shipped, shipped_size);  // optional: a list shipped with the app
 *     jsui_gpu_set_pipeline_cache(gpu, "<app data>/jsui-pipelines.vulkan.bin");
 *     ... render as usual: new pipelines are appended to the file as they appear
 *
 * - Precompiling never blocks a frame. Vulkan and Metal precompile on a
 *   worker thread of the context, as does Dawn when the device was created
 *   with WGPUFeatureName_ImplicitDeviceSynchronization (Dawn then locks the
 *   device itself). Other Dawn devices and the browser are not thread safe:
 *   there the context precompiles for ~2 ms after a render (once per frame
 *   however many surfaces it renders), on the render thread, unless the host
 *   schedules it with jsui_gpu_precompile_step (e.g. while idle). A frame
 *   that needs a pipeline before its turn compiles it as it would without
 *   the cache.
 * - Files and lists hold Skia's serialized pipeline keys behind a header
 *   naming the Skia build and the backend. A file of another Skia build or
 *   backend is started afresh, a damaged tail (a crash while appending) is
 *   dropped, a file that is not a pipeline cache is left alone (the call
 *   fails); each with one log line and nothing else. Use one file per
 *   backend. Keys are fitted to the device before they are precompiled: a
 *   list recorded on another GPU (one shipped with the app) gets this
 *   device's depth/stencil formats, and a key the device cannot build is
 *   skipped (counted as failed).
 * - These calls are thread safe (they do not use the device), except
 *   jsui_gpu_precompile_step: call it on the thread that renders.
 * - Development: the environment variable JSUI_PIPELINE_CACHE names a file
 *   that every Graphite context uses in place of the host's;
 *   JSUI_LOG_PIPELINES=1 logs every pipeline a context creates,
 *   JSUI_LOG_PIPELINES=compiled only those a frame compiled (new: not in
 *   the cache yet; known: still queued for the precompile).
 */
enum {
    JSUI_PRECOMPILE_NONE = 0,   /* no pipeline cache (GL contexts) */
    JSUI_PRECOMPILE_THREAD = 1, /* on a worker thread */
    JSUI_PRECOMPILE_SLICED = 2  /* in slices on the render thread */
};

typedef struct JsuiPipelineStats {
    int mode;                         /* JSUI_PRECOMPILE_* */
    int queued;                       /* keys waiting to be precompiled, the one compiling included */
    unsigned long long precompiled;   /* keys precompiled from files / lists (also those a frame
                                         created first: then a cache hit) */
    unsigned long long failed;        /* keys this Skia cannot decode or this device cannot build */
    unsigned long long compiled;      /* pipelines compiled on demand inside a frame (cache misses) */
    unsigned long long added;         /* ... of them new: recorded (appended to the file) */
    unsigned long long unserializable; /* ... of them that Skia cannot record */
    unsigned long long known;         /* distinct pipelines recorded and queued */
} JsuiPipelineStats;

/* Loads the cache file at `path` (UTF-8; created when missing), queues its
 * pipelines for precompiling, and from now on appends every new pipeline to
 * it, including those the context created before the call. NULL / "" stops
 * recording. Returns the number of pipelines queued, -1 when the file cannot
 * be created / read or is not a pipeline cache, or `gpu` has no pipeline
 * cache (GL, a WASM build: no file system). */
JSUI_API int jsui_gpu_set_pipeline_cache(JsuiGpuContext* gpu, const char* path);
/* Queues the pipelines of a list (the bytes of a cache file, or of
 * jsui_gpu_save_pipelines) that the context does not know yet. Returns the
 * number queued, -1 when the list is not one for this Skia build and backend. */
JSUI_API int jsui_gpu_precompile_pipelines(JsuiGpuContext* gpu, const void* data, size_t size);
/* Every pipeline the context knows (recorded or queued) as a list; free
 * *out_data with jsui_free(). 0 on success, -1 without a pipeline cache. */
JSUI_API int jsui_gpu_save_pipelines(JsuiGpuContext* gpu, void** out_data, size_t* out_size);
/* JSUI_PRECOMPILE_SLICED: precompiles queued pipelines for about `budget_ms`
 * (at least one) and leaves the scheduling to the host from now on (no more
 * slices after renders). Returns the number still queued (also in the other
 * modes, where it does nothing else). */
JSUI_API int jsui_gpu_precompile_step(JsuiGpuContext* gpu, double budget_ms);
JSUI_API void jsui_gpu_get_pipeline_stats(JsuiGpuContext* gpu, JsuiPipelineStats* out);

/* ShaderView sources compiled and warmed ahead of use (precompileShaders()
 * in JS does the same): compiles `sksl` (UTF-8) now and draws it once
 * off-screen in the next frame rendered on a GPU, so the GPU builds its
 * pipelines in that frame instead of the one where a ShaderView first shows
 * it. Call it while loading or idle. `images_json` (NULL: none) is the
 * view's `images` prop as JSON (which children are images or textures, and
 * their fit / tile), so the same kind of child is sampled. jsui_needs_redraw
 * stays 1 until that frame. Returns 0, or -1 when the shader does not
 * compile (logged as a warning). */
JSUI_API int jsui_precompile_shader(JsuiRuntime* rt, const char* sksl, const char* images_json);

/* ------------------------------------------------------------- GPU (Metal)
 *
 * Skia Ganesh on Metal: draws the frame straight into a Metal texture (a
 * CAMetalLayer / MTKView drawable, or an engine's render target). Available
 * when the library was built with JSUI_METAL (iOS builds by default);
 * otherwise jsui_metal_create() returns NULL and the host falls back to
 * jsui_render_rgba(). Handles are Objective-C objects passed as void*
 * ((__bridge void*) under ARC); JSUI retains what it keeps.
 *
 *     JsuiMetalContext* mtl = jsui_metal_create((__bridge void*)device, (__bridge void*)queue);
 *     // per frame:
 *     jsui_tick(rt, now_ms);
 *     id<CAMetalDrawable> d = [layer nextDrawable];
 *     jsui_render_metal(rt, mtl, (__bridge void*)d.texture, MTLPixelFormatBGRA8Unorm, w, h, 0);
 *     id<MTLCommandBuffer> cb = [queue commandBuffer];  // same queue: runs after JSUI's work
 *     [cb presentDrawable:d]; [cb commit];
 *
 * - JSUI submits its work on `queue`; anything the host encodes on the same
 *   queue afterwards (present, compositing the UI texture) is ordered after it.
 * - The target is top-left origin and must be usable as a render target
 *   (MTLTextureUsageRenderTarget). Formats: BGRA8Unorm (0 means this),
 *   RGBA8Unorm, the _sRGB variants, RGB10A2Unorm, BGR10A2Unorm, RGBA16Float.
 * - Like the GL path it always repaints the whole target; flags take
 *   JSUI_GL_NO_CLEAR to draw over the texture's contents.
 * - One context may serve any number of runtimes; all calls on one thread.
 */
typedef struct JsuiMetalContext JsuiMetalContext;

/* `device`: id<MTLDevice>, `queue`: id<MTLCommandQueue> of that device.
 * Returns NULL when the library was built without JSUI_METAL. */
JSUI_API JsuiMetalContext* jsui_metal_create(void* device, void* queue);
/* Waits for submitted work, then frees the context and its GPU resources. */
JSUI_API void jsui_metal_destroy(JsuiMetalContext* mtl);
/* Frees cached GPU resources not in use (memory warnings); `cache_bytes` > 0
 * also sets the cache budget (default 256 MB). */
JSUI_API void jsui_metal_purge(JsuiMetalContext* mtl, size_t cache_bytes);
/* Paints the current frame into `texture` (id<MTLTexture>, `format` its
 * MTLPixelFormat) at width x height device px. Lays out first and clears
 * needs_redraw. Returns 0 on success, -1 on failure. */
JSUI_API int jsui_render_metal(JsuiRuntime* rt, JsuiMetalContext* mtl, void* texture, unsigned int format, int width,
                               int height, int flags);

/* Encode the current frame as PNG (for tests / screenshots). Caller frees with jsui_free(). */
JSUI_API int jsui_render_png(JsuiRuntime* rt, void** out_data, size_t* out_size);
JSUI_API void jsui_free(void* p);

/* ----------------------------------------------------------------- Input
 *
 * Pointer coordinates are logical px relative to the viewport's top-left.
 *
 * Each jsui_send_* function returns non-zero if the UI consumed the event, so
 * the game processes the input itself only when it gets 0 (e.g. a click on a
 * part of a HUD that nothing in the UI handles goes through to the world).
 * Consumed means something in the UI captured it, not that the point is over
 * painted UI:
 *  - pointer / touch: the hit element or an ancestor has a press, pointer,
 *    hover or responder handler (onPress, onPointerDown, onHoverIn,
 *    onStartShouldSetResponder, PanResponder, ...) or is a TextInput, or JS
 *    called preventDefault(). A touch on a ScrollView that can scroll is
 *    consumed too. Views with pointerEvents 'none' / 'box-none' are not hit
 *    themselves, so full-screen containers let input through. A decorative
 *    backdrop that should block the game needs a handler, e.g.
 *    onStartShouldSetResponder={() => true}.
 *  - a press decides its whole gesture: the moves while buttons are held and
 *    the release are consumed exactly when the press was (a drag the game
 *    started over empty UI stays the game's, even over buttons).
 *  - wheel: JS handled it or a ScrollView scrolled.
 *  - keys / gamepad buttons: a JS handler called preventDefault(), or a
 *    default action ran (focus moved, a focused element was activated, a
 *    ScrollView scrolled, a TextInput edited). The repeats and the release of
 *    a consumed press are consumed too; those of a press the UI passed on are
 *    not.
 *  - sticks: focus moved, a ScrollView scrolls, or JS handled it.
 * jsui_hit_test answers the pointer question before any event is sent, for
 * hosts that route input by hit testing (a Godot Control's _has_point, a web
 * overlay deciding whether the page below gets an event).
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

/* When the input events sent next happened, in ms on the runtime clock (the
 * clock of jsui_tick's now_ms; in a browser, DOM event.timeStamp next to
 * requestAnimationFrame timestamps). It stamps the events (`timestamp`,
 * e.timeStamp in JS) sent after this call until the next jsui_tick, so
 * gesture velocities (PanResponder vx / vy, ScrollView flings) see the real
 * spacing of events that arrive between two frames. Without it events carry
 * the time of the last tick. Timestamps never go backwards. NaN clears it. */
JSUI_API void jsui_set_event_time(JsuiRuntime* rt, double time_ms);

/* jsui_hit_test flags. */
enum {
    JSUI_HIT_POINTER = 1 << 0, /* presses / pointer input at the point are captured */
    JSUI_HIT_SCROLL = 1 << 1   /* a ScrollView that can scroll (or a wheel handler) is there */
};
/* What the UI would capture at (x, y) (logical px): JSUI_HIT_* flags, 0 when
 * input there passes through to whatever is behind the UI. Uses the current
 * layout and sends no events. */
JSUI_API int jsui_hit_test(JsuiRuntime* rt, float x, float y);
/* jsui_hit_test for the whole view at once, for hosts that must answer it
 * where the runtime is not (the web host's worker mode answers on the page's
 * thread from the last copy): writes up to `max_regions` regions to `out`,
 * 5 floats each (x, y, width, height in logical px, then the JSUI_HIT_*
 * flags), bottom to top. The last region containing a point gives its
 * flags; none: 0. Approximate where jsui_hit_test is exact: transformed
 * views give their bounding box, rounded corners are ignored, and text
 * with pointer handlers on some of its spans captures its whole box.
 * Returns the number of regions (call again with a bigger buffer when it
 * exceeds `max_regions`). Uses the current layout. */
JSUI_API int jsui_hit_regions(JsuiRuntime* rt, float* out, int max_regions);

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
