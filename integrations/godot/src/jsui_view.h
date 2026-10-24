// JsuiView: a Godot Control that runs a JSUI bundle and shows it.
//
// The runtime ticks in NOTIFICATION_PROCESS and paints only when something
// changed, with one of two renderers (chosen in the first render from the
// RenderingServer's driver; `force_cpu` pins the CPU one):
//
//   * GPU (Vulkan / Metal / WebGPU on godot-aw3, jsui_gpu_render.cpp): Skia Graphite draws into a
//     texture of Godot's RenderingDevice on the render thread, repainting
//     only the damaged rect, which Godot then copies into the texture it
//     samples (a Texture2DRD). The views of one runtime share one Skia
//     context (the runtime view's). With a separate render thread the
//     frame's renders of all of a runtime's views are queued there as one
//     job and the main thread goes on; whatever touches the runtime next
//     waits for that job (rt_, see RuntimeRef).
//   * CPU: jsui_render_rgba_incremental into a buffer that persists between
//     frames, uploaded to an ImageTexture when pixels changed (the
//     Compatibility renderer, headless runs, or when the GPU path fails).
//
// The texture is drawn by an internal canvas item with premultiplied-alpha
// blending (JSUI produces premultiplied RGBA), so the node's own `material`
// stays free for the game.
//
// Several views can share one runtime (one bundle and JS context), like
// React Native root views: a view whose `runtime_view` points at another
// JsuiView shows a surface of that view's runtime (jsui_surface_create)
// running the AppRegistry component `component` with `initial_props`. The
// runtime view ticks the runtime once per frame and then paints every
// surface view whose frame changed; it also emits the runtime's signals
// (messages, state, calls, errors), and the state / call / message methods
// of a surface view act on it. `component` on the runtime view itself picks
// the main surface's component (empty: the bundle's first registered one).
// `auto_size` sizes the view to its component's content (its minimum size),
// for small widgets inside existing Godot UIs.
//
// Host textures (set_texture, jsui_set_texture): Godot textures the UI shows
// with <HostTexture name="..."/> (a VideoStreamPlayer's frames, a SubViewport,
// any Texture2D). They belong to the runtime (the runtime view keeps them,
// across restarts). With the GPU renderer each is copied on the GPU into a
// texture of the view's own, which Skia samples (jsui_gpu_render.cpp); with
// the CPU renderer its pixels go through get_image().
//
// Audio (jsui_audio.h): the runtime view plays its runtime's UI sounds
// (Sound.play, useMusic, <UISounds>, ...) with AudioStreamPlayers of its own
// on Godot's audio buses, so they follow the game's volume settings and
// effects. A JSUI bus plays on the Godot bus `audio_bus_map` names for it,
// else on the Godot bus of the same name, else on `audio_bus`. The UI's own
// bus volumes scale its voices, never Godot's buses. `audio_enabled` off
// plays nothing (every voice fails).
//
// GPU pipeline cache (jsui_gpu_set_pipeline_cache): the runtime's Skia
// context records every GPU pipeline it creates in the runtime view's
// `pipeline_cache_path`, with the renderer's name before the extension
// (user://jsui-pipelines.bin -> user://jsui-pipelines.vulkan.bin), and the
// next start precompiles them, plus a list shipped at the same path under
// res:// (res://jsui-pipelines.vulkan.bin), off the frame.
//
// Fonts (register_font, jsui_register_font): FontFile resources, bytes or
// font files the UI's fontFamily can name. Like host textures they belong to
// the runtime: the runtime view registers the `fonts` property and every
// register_font / unregister_font / fallback / default-family call on each
// new runtime (start()), in order, so calls made before start() and across
// restarts apply; a surface view forwards them to its runtime view.
#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "jsui/jsui.h"

namespace jsui_godot {

class ViewAudio;

class JsuiView : public godot::Control {
  GDCLASS(JsuiView, godot::Control)

 public:
  JsuiView();
  ~JsuiView() override;

  // Properties.
  void set_bundle_url(const godot::String& url);
  godot::String get_bundle_url() const { return bundle_url_; }
  void set_dev_mode(bool on);
  bool get_dev_mode() const { return dev_mode_; }
  void set_pixel_ratio(float ratio);
  float get_pixel_ratio() const { return pixel_ratio_; }
  void set_autostart(bool on) { autostart_ = on; }
  bool get_autostart() const { return autostart_; }
  void set_forward_gamepad(bool on) { forward_gamepad_ = on; }
  bool get_forward_gamepad() const { return forward_gamepad_; }
  void set_js_frame_budget_ms(double ms);
  double get_js_frame_budget_ms() const { return js_frame_budget_ms_; }
  void set_input_pass_through(bool on) { input_pass_through_ = on; }
  bool get_input_pass_through() const { return input_pass_through_; }
  void set_force_cpu(bool on);
  bool get_force_cpu() const { return force_cpu_; }
  void set_runtime_view(const godot::NodePath& path);
  godot::NodePath get_runtime_view() const { return runtime_view_; }
  void set_component(const godot::String& key);
  godot::String get_component() const { return component_; }
  void set_initial_props(const godot::Dictionary& props);
  godot::Dictionary get_initial_props() const { return initial_props_; }
  void set_auto_size(bool on);
  bool get_auto_size() const { return auto_size_; }
  // Applies when the runtime's Skia context is created (the first GPU
  // frame); "" for none. Surface views use their runtime view's.
  void set_pipeline_cache_path(const godot::String& path) { pipeline_cache_path_ = path; }
  godot::String get_pipeline_cache_path() const { return pipeline_cache_path_; }
  // 0: the runtime's clock (animations, timers, scrolling) is the wall
  // clock. N > 0: each frame advances it by 1/N s however long frames take,
  // so the UI draws the frames of a machine running at N fps, the same on
  // any machine (tests, recording a pipeline list). The runtime view's applies.
  void set_fixed_fps(int fps) { fixed_fps_ = fps > 0 ? fps : 0; }
  int get_fixed_fps() const { return fixed_fps_; }
  // Audio (the runtime view's apply; see the top of this file).
  void set_audio_enabled(bool on);
  bool get_audio_enabled() const { return audio_enabled_; }
  void set_audio_bus(const godot::String& bus) { audio_bus_ = bus; }
  godot::String get_audio_bus() const { return audio_bus_; }
  void set_audio_bus_map(const godot::Dictionary& map) { audio_bus_map_ = map; }
  godot::Dictionary get_audio_bus_map() const { return audio_bus_map_; }
  // Fonts registered (family from the file) on every new runtime. Assigning
  // the property registers the fonts it adds on a running one; fonts it
  // drops stay registered until the next start().
  void set_fonts(const godot::Array& fonts);
  godot::Array get_fonts() const { return fonts_; }

  // Methods.
  void start();   // (re)creates the runtime and loads bundle_url (surface views: the surface)
  void stop();    // destroys the runtime (surface views: the surface)
  void reload();  // re-evaluates the last bundle (keeps the runtime)
  bool is_running() const { return rt_.unsynced() != nullptr; }
  void post_message(const godot::String& name, const godot::Variant& data);
  void post_message_json(const godot::String& name, const godot::String& json);

  // Shared state (jsui_set_state / jsui_patch_state / jsui_get_state). Values
  // are JSON-encoded; null removes the key. Kept across start()/stop():
  // the view re-applies them to every new runtime.
  int set_state(const godot::String& key, const godot::Variant& value);
  int set_state_json(const godot::String& key, const godot::String& json);
  int patch_state(const godot::String& key, const godot::Variant& patch);
  int patch_state_json(const godot::String& key, const godot::String& json);
  godot::Variant get_state(const godot::String& key) const;
  godot::String get_state_json(const godot::String& key) const;

  // Frame data (jsui_set_buffer): numbers pushed every frame without JSON.
  // `data` is a PackedFloat32Array / PackedFloat64Array / PackedInt32Array /
  // PackedByteArray, or a PackedVector2Array / PackedVector3Array /
  // PackedColorArray (their components, in order). JS reads it with
  // useHostBuffer / useHostBufferValue. Returns 1 changed, 0 unchanged, -1
  // for an unsupported type. Set before start(): applied when it starts.
  int set_buffer(const godot::String& key, const godot::Variant& data);
  int remove_buffer(const godot::String& key);

  // Host textures (jsui_set_texture): a Godot Texture2D shown by the UI's
  // <HostTexture name="..."/> (useHostTexture(name) gives its size), e.g.
  // VideoStreamPlayer.get_video_texture() or a SubViewport's texture. `live`:
  // its contents change every frame (a playing video, a viewport that renders
  // every frame), so the view copies it every frame; otherwise only when the
  // texture emits `changed` (ImageTexture.update / set_image) or after
  // update_texture(). Kept across start() / stop() and re-applied to new
  // runtimes. GPU renderer: copied on the GPU into a texture of the view's
  // own that Skia samples (jsui_gpu_render.cpp); CPU renderer (and formats
  // the GPU path cannot take): get_image() -> jsui_set_texture_pixels.
  // Returns 1 changed, 0 unchanged (a null texture removes it).
  int set_texture(const godot::String& name, const godot::Ref<godot::Texture2D>& texture, bool live);
  int remove_texture(const godot::String& name);  // 1 if it existed
  int update_texture(const godot::String& name);  // new contents: 1, or -1 for an unknown name

  // Audio: stops the UI's voices (of one JSUI bus, or all for ""), fading
  // out over fade_ms; the UI hears they were stopped. get_audio_voices lists
  // the playing ones (voice, url, bus, godot_bus, player, volume, pitch,
  // loop, paused, loading). resolve_audio_bus: the Godot bus a JSUI bus
  // plays on now.
  void stop_all_sounds(const godot::String& bus, float fade_ms);
  godot::Array get_audio_voices() const;
  godot::String resolve_audio_bus(const godot::String& bus) const;
  // Fonts (jsui_register_font and friends), for the runtime (every view of
  // it). `font`: a FontFile (or a FontVariation of one; its data), a
  // PackedByteArray, or a path (res://, user://, absolute) of a TTF / OTF /
  // TTC / WOFF / WOFF2 file. `family` "": the family name in the file;
  // weight <= 0 / italic < 0: the file's, else overrides (1..1000, 0 / 1).
  // Remembered and re-applied to every new runtime. False when the font
  // cannot be read or (with a runtime) is not a font.
  bool register_font(const godot::String& family, const godot::Variant& font, int weight, int italic);
  // Removes the family's faces; true if it was registered (without a
  // runtime: if a queued registration of it was dropped).
  bool unregister_font(const godot::String& family);
  // Families tried in order for glyphs the styled font lacks (emoji, CJK);
  // empty clears the list.
  void set_font_fallbacks(const godot::PackedStringArray& families);
  // The family of text whose style names none (default "Inter").
  void set_default_font_family(const godot::String& family);

  // Calls: Host.call() -> call_requested signal -> resolve_call / reject_call;
  // call_ui() -> Host.handle() handler -> callback(ok, result) + ui_call_completed.
  int resolve_call(int64_t id, const godot::Variant& result);
  int resolve_call_json(int64_t id, const godot::String& json);
  int reject_call(int64_t id, const godot::String& message);
  int64_t call_ui(const godot::String& name, const godot::Variant& args, const godot::Callable& callback);
  godot::Ref<godot::Texture2D> get_texture() const;
  godot::Ref<godot::Image> get_frame_image();
  // The current UI state painted by the CPU raster (tests: compare it with
  // get_frame_image() from the GPU path). Renders first if a frame is due.
  godot::Ref<godot::Image> get_cpu_frame_image();
  godot::String get_renderer() const;  // "vulkan", "metal", "cpu" or "" before the first frame
  godot::Dictionary get_render_stats() const;
  // Profiling (jsui_set_profiling / jsui_get_perf): time per frame phase and
  // work counters since the last reset (`reset` starts new totals).
  void set_profiling(bool on);
  bool get_profiling() const { return profiling_; }
  godot::Dictionary get_perf(bool reset);
  int get_pending_loads() const;
  godot::String get_last_error() const;
  float get_effective_pixel_ratio() const { return ratio_; }
  // The React Native root tag of the view's surface (1: a runtime's main
  // surface; 0 while not running).
  int get_root_tag() const { return rt_ ? jsui_surface_tag(rt_) : 0; }
  // jsui_hit_test at a point in the view's local coordinates: HIT_POINTER /
  // HIT_SCROLL flags, 0 where input passes through the UI.
  int hit_test(const godot::Vector2& position) const;
  static constexpr int HIT_POINTER = JSUI_HIT_POINTER;
  static constexpr int HIT_SCROLL = JSUI_HIT_SCROLL;
  // sound_ended's status (jsui_audio_voice_ended).
  static constexpr int SOUND_FINISHED = JSUI_AUDIO_FINISHED;
  static constexpr int SOUND_STOPPED = JSUI_AUDIO_STOPPED;
  static constexpr int SOUND_FAILED = JSUI_AUDIO_FAILED;

  void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
  // With input_pass_through, only points where the UI captures input are
  // inside the view, so Godot hands the rest to the controls behind it and to
  // the game (_unhandled_input), and never gives the view focus for them.
  bool _has_point(const godot::Vector2& point) const override;
  void _notification(int what);

 protected:
  static void _bind_methods();

 private:
  static void onMessage(void* user, const char* name, const char* json);
  static void onState(void* user, const char* key, const char* json);
  static void onCall(void* user, uint64_t id, const char* name, const char* args);
  static void onCallCancel(void* user, uint64_t id);
  static void onCallResult(void* user, uint64_t id, int ok, const char* result);
  static void onLog(void* user, JsuiLogLevel level, const char* message);
  static void onLoad(void* user, JsuiRuntime* rt, uint64_t id, const char* url);

  void process();
  // After the runtime's tick: cursor, auto size and whether the frame changed
  // (process() then paints the views that changed).
  void afterTick();
  void updateViewport();
  void updateAutoSize();
  // Surface views (runtime_view): attach to / detach from the runtime view.
  JsuiView* resolveRuntimeView() const;
  void attachToRuntimeView();
  void detachFromRuntimeView();
  void createSurface();   // the runtime view is running: show our component
  void destroySurface();  // frees the surface (the runtime stays)
  void surfaceGone();     // the runtime went away (its handles are freed)
  bool showsOurRuntime(const JsuiView* v) const;  // this view or one of surfaces_
  std::string propsJson() const;
  void render();
  void renderCpu();
  // GPU path (jsui_gpu_render.cpp). renderGpu returns false when the GPU
  // path failed and the view must fall back to the CPU.
  enum class Renderer { Unknown, Cpu, Vulkan, Metal, WebGPU, D3D12 };
  void chooseRenderer();
  // On the runtime view: its Skia context, which its surface views share
  // (created on first use; false when there is none).
  bool createGpuContext(Renderer want, const godot::String& driver, godot::Object* interop);
  // The new context's pipeline cache (jsui_view.cpp): the shipped list and
  // the file for `renderer` ("vulkan", ...).
  void setupPipelineCache(const godot::String& renderer);
  JsuiView* runtimeView() { return host_ ? host_ : this; }
  bool renderGpu();
  void releaseGpu();
  // Host textures (set_texture). On the runtime view. syncTextures (main
  // thread, before the tick) picks how they reach the runtime (GPU copies or
  // CPU pixels), adopts GPU copies made in earlier frames and decides this
  // frame's copies; doTextureWork (render thread, jsui_gpu_render.cpp) makes
  // them. With a separate render thread the work goes with the runtime's
  // frame job, or as a job of its own on the same fence (queueTextureJob).
  void syncTextures();
  void queueTextureJob();
  void runTextureJob();
  void doTextureWork();
  void releaseTextureGpu();  // frees every GPU copy and the layout pass (blocking)
  void onTextureChanged(const godot::String& name);
  void runOnRenderThread(const std::function<void()>& job);
  void runRenderJob();
  // The steady-state frame (with a separate render thread): batchFrame adds
  // the view to the runtime view's frame job, queueFrameJob queues that on
  // the render thread (once per frame, after the views painted), and
  // runFrameJob paints each view in it there (paintGpuJob) without the main
  // thread waiting.
  void batchFrame(JsuiView* view);
  void queueFrameJob();
  void runFrameJob();
  void paintGpuJob();
  // Main thread: waits until the render thread has run every job queued on
  // the runtime's fence, the other views' too (returns at once when it has,
  // or on the render thread itself).
  void waitRender();
  // Main thread: takes the queued job's result (frame_rendered, stats);
  // false when it failed (then gpuFailed()).
  bool collectGpuRender();
  // The runtime view, before its tick: collectGpuRender for itself and each
  // surface view (a failed one falls back to the CPU renderer).
  void collectFrames();
  void gpuFailed();
  void finishGpuFrame(const JsuiRect& dirty);
  godot::PackedByteArray readGpuFrame();
  void showTexture(const godot::RID& texture);
  void updateCursor();
  void flushMessages();
  void reportErrors();
  void releaseHeldInput();
  void onJoyConnectionChanged(int64_t device, bool connected);
  // Fonts. A FontOp is one register_font / unregister_font call, kept (on
  // the runtime view, or a surface view until it attaches) and replayed on
  // every new runtime after the `fonts` property.
  struct FontOp {
    bool unregister = false;
    std::string family;
    godot::PackedByteArray data;
    int weight = 0, italic = -1;
  };
  bool applyFontOp(FontOp op);  // applies it to the runtime (if any) and logs it
  bool runFontOp(const FontOp& op);  // jsui_register_font / jsui_unregister_font on rt_
  void applyFontSettings();          // fallbacks and default family, on rt_
  void startFonts();                 // a new runtime: the property, the log, the settings
  void forwardFonts();               // surface view: its fonts and queued calls go to host_
  bool handleGamepad(const godot::Ref<godot::InputEvent>& event);
  float computeRatio() const;
  double nowMs() const;
  double clockMs();

  godot::String bundle_url_;
  bool dev_mode_ = false;
  float pixel_ratio_ = 0.f;  // 0: follow the canvas -> screen scale
  bool autostart_ = true;
  bool forward_gamepad_ = true;
  double js_frame_budget_ms_ = 5;
  bool input_pass_through_ = true;
  int mouse_buttons_ = 0;  // buttons whose press reached the UI (DOM bits)
  bool force_cpu_ = false;
  godot::NodePath runtime_view_;
  godot::String component_;
  godot::Dictionary initial_props_;
  bool auto_size_ = false;
  bool profiling_ = false;
  godot::String pipeline_cache_path_ = "user://jsui-pipelines.bin";
  int fixed_fps_ = 0;
  godot::String pipeline_file_;  // the runtime view's cache file in use (an OS path), "" for none
  bool audio_enabled_ = true;
  godot::String audio_bus_ = "Master";
  godot::Dictionary audio_bus_map_;
  // The runtime's audio handler (runtime views; made with the first runtime).
  std::unique_ptr<ViewAudio> audio_;

  // The runtime handle this view renders and sends input to: the runtime it
  // owns (its main surface), or a surface of host_'s runtime. A GPU render
  // queued on Godot's render thread (a separate one: jsui_gpu_render.cpp)
  // uses the runtime until it has run, and so does every other view's render
  // queued on the same runtime. Reading rt_ on the main thread therefore
  // first waits for all of them (waitRender, on the runtime's fence; Godot
  // syncs with its render thread after process every frame, so in steady
  // state they have run by the next frame). Every jsui_*(rt_, ...) call is
  // safe that way, also for surface views and calls forwarded to host_.
  class RuntimeRef {
   public:
    explicit RuntimeRef(JsuiView* view) : view_(view) {}
    RuntimeRef& operator=(JsuiRuntime* rt) {
      view_->waitRender();
      rt_ = rt;
      return *this;
    }
    operator JsuiRuntime*() const {
      view_->waitRender();
      return rt_;
    }
    // The handle without waiting: for the render job itself, and to test
    // whether there is one (only the main thread changes it).
    JsuiRuntime* unsynced() const { return rt_; }

   private:
    JsuiView* view_;
    JsuiRuntime* rt_ = nullptr;
  };
  RuntimeRef rt_{this};
  // Buffers set while no runtime runs (type, bytes), applied by start().
  struct PendingBuffer {
    JsuiBufferType type;
    std::vector<uint8_t> bytes;
    size_t count;
  };
  std::vector<std::pair<std::string, PendingBuffer>> pendingBuffers_;
  // Fonts: the `fonts` property, the entries of it the runtime (or host_,
  // for a surface view) got, the calls to replay, and the settings.
  godot::Array fonts_, fonts_applied_;
  JsuiView* fonts_host_ = nullptr;  // the runtime view fonts_applied_ went to (surface view)
  std::vector<FontOp> font_ops_;
  bool font_fallbacks_set_ = false, default_font_set_ = false;
  std::string font_fallbacks_, default_font_family_;
  // Host textures (set_texture), on the runtime view; kept across restarts.
  // With the GPU renderer each one is copied (RenderingDevice.texture_copy,
  // recorded into Godot's frame) into a mirror: a RenderingDevice texture of
  // ours, which is what the runtime samples (jsui_set_texture). Godot thus
  // keeps doing every barrier, layout and lifetime of its own texture (a
  // video frame upload, a SubViewport render, an ImageTexture replaced), and
  // the mirror lives as long as JSUI may use it. A mirror made in frame N is
  // handed to the runtime in frame N + 1 (adopted), once Godot has submitted
  // the frame with its first copy; a replaced one is freed after that.
  struct TextureMirror {
    godot::RID rd;        // our RenderingDevice texture
    godot::RID uniforms;  // Vulkan: the layout pass's uniform set for it
    void* handle = nullptr;
    unsigned format = 0;  // VkFormat / MTLPixelFormat / 0 (WebGPU: the texture's own)
    int64_t data_format = 0;  // RenderingDevice::DataFormat
    int w = 0, h = 0;
    uint64_t created_frame = 0;
    bool valid() const { return rd.is_valid(); }
  };
  struct HostTexture {
    std::string key;
    godot::String name;
    godot::Ref<godot::Texture2D> texture;
    godot::Callable changed;  // connected to the texture's `changed`
    bool live = false;
    bool dirty = true;      // new contents since they were last taken
    int flags = 0;          // JSUI_TEXTURE_*
    bool pixels_set = false;  // the runtime holds CPU pixels of it
    // GPU copies. The render thread reads `rs` / `copy` and writes the rest
    // but `cur` (the main thread adopts `next` into it); the main thread
    // touches them only after waitRender().
    godot::RID rs;          // the Texture2D's RenderingServer texture
    bool copy = false;      // copy it this frame (doTextureWork)
    godot::RID src;         // its RenderingDevice texture, last seen
    bool src_ok = false;
    int64_t mirror_format = 0;
    int src_w = 0, src_h = 0;
    TextureMirror cur, next;  // cur: what the runtime samples; next: a new one, adopted next frame
    bool gpu_set = false;     // the runtime holds cur
    int gpu_flags = 0;        // the flags it was set with
    bool shown_pending = false;  // a copy into cur the runtime has not been told about
    bool cpu_only = false;    // the GPU path cannot take it (why): CPU pixels instead
    std::string why;
    bool warned = false;
  };
  std::vector<HostTexture> textures_;
  std::vector<TextureMirror> retired_mirrors_;  // freed by the next doTextureWork
  enum class TextureMode { None, Pixels, Gpu };
  TextureMode texture_mode_ = TextureMode::None;
  Renderer gpu_api_ = Renderer::Unknown;  // the API of gpu_
  bool texture_work_ = false;        // doTextureWork is due (main thread)
  bool frame_job_textures_ = false;  // the queued frame job does it
  uint64_t texture_frame_ = 0;       // Engine.get_frames_drawn() of this frame's sync
  uint64_t texture_job_id_ = 0;
  // Vulkan: Skia samples a mirror in SHADER_READ_ONLY_OPTIMAL (jsui.h), but
  // Godot's render graph leaves a copy destination in TRANSFER_DST_OPTIMAL.
  // After each copy a one-texel compute dispatch samples the mirror, so the
  // graph moves it to SHADER_READ_ONLY_OPTIMAL itself, in the same frame.
  struct LayoutPass {
    godot::RID shader, pipeline, sampler, sink;
    bool failed = false;
  };
  LayoutPass layout_pass_;
  bool uploadTexturePixels(HostTexture& e);  // CPU pixels of e (get_image)
  JsuiView* host_ = nullptr;             // the runtime view, for a surface view
  std::vector<JsuiView*> surfaces_;      // surface views of this view's runtime
  bool attach_wanted_ = false;           // a surface view that should show its surface
  bool viewport_auto_ = false;           // jsui_set_viewport(0, 0) applied
  uint64_t start_usec_ = 0;
  double clock_ms_ = 0, clock_wall_ms_ = 0;  // the runtime's clock (clockMs), and nowMs() then
  int logical_w_ = 0, logical_h_ = 0, pixel_w_ = 0, pixel_h_ = 0;
  float ratio_ = 1.f;
  bool render_pending_ = true;

  // CPU renderer.
  std::vector<uint8_t> pixels_;   // jsui's incremental target (stable address)
  int pixels_w_ = 0, pixels_h_ = 0;
  godot::PackedByteArray upload_; // staging copy handed to the Image
  godot::Ref<godot::ImageTexture> texture_;

  // GPU renderer. `src` is Skia's retained render target (only JSUI draws
  // into it); `dst` is what the canvas item samples; Godot copies the damage
  // from one to the other, so Godot's RenderingDevice tracks dst's layout and
  // barriers itself. A new pair (start, resize) is used from the frame after
  // its creation, once Godot has run the initial upload and priming copy.
  Renderer renderer_ = Renderer::Unknown;
  // The runtime's Skia context, on the runtime view (surface views use their
  // runtime view's: one per runtime). It goes with the runtime view, or
  // when the runtime view leaves the GPU path while no surface view is
  // attached, after the runtime's queued renders have run.
  JsuiGpuContext* gpu_ = nullptr;
  struct GpuTextures {
    godot::RID src, dst;
    int w = 0, h = 0;
    uint64_t created_frame = 0;
    bool valid() const { return src.is_valid(); }
  };
  GpuTextures gpu_cur_, gpu_next_;
  bool gpu_retained_ = false;  // gpu_cur_.src holds the last frame (incremental repaint)
  godot::Ref<godot::Texture2DRD> gpu_texture_;
  std::function<void()> render_job_;
  // Render jobs queued on a runtime, in order: the runtime view numbers the
  // frame jobs of all its views on its fence, and the render thread runs
  // them in that order, so done == queued means all of them have run. A
  // runtime view outlives the jobs on its fence: stop() and the destructor
  // wait for them first.
  struct Fence {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<uint64_t> queued{0}, done{0};  // job ids
  };
  Fence fence_;  // ours while we own the runtime (or have none)
  Fence& runtimeFence() { return host_ ? host_->fence_ : fence_; }
  // This view's part of a frame job (batchFrame): the main thread fills it,
  // the render thread paints it and fills in ok / dirty, the main thread
  // reads those back once the job's fence has reached id.
  struct GpuJob {
    JsuiRuntime* rt = nullptr;
    JsuiGpuContext* gpu = nullptr;
    godot::RID src, dst;
    JsuiGpuTarget target{};  // all but .texture (the render thread looks src up)
    Fence* fence = nullptr;  // the runtime's, signalled when the job has run
    uint64_t id = 0;
    bool ok = false;
    JsuiRect dirty{0, 0, 0, 0};
  };
  GpuJob gpu_job_;
  bool gpu_job_pending_ = false;  // queued, result not collected yet (main thread)
  // The runtime view's frame job: the views batched while process() paints
  // (main thread), and the ones of the queued job (read by the render thread
  // until the fence reaches frame_job_id_). No game code runs while views
  // are batched (any other render queues the batch first), so none of them
  // can go meanwhile.
  std::vector<JsuiView*> frame_batch_, frame_job_views_;
  bool batching_ = false;
  uint64_t frame_job_id_ = 0;
  // Hand-off stats (get_render_stats): main-thread waits on the render
  // thread, frames handed to it, the runtime's render-thread frame jobs, and
  // the time the frames and jsui_render_gpu_target in them take.
  uint64_t blocking_waits_ = 0, main_wait_usec_total_ = 0, gpu_jobs_ = 0, render_thread_jobs_ = 0;
  std::atomic<uint64_t> gpu_job_usec_total_{0}, jsui_render_usec_total_{0};
  // Stats (get_render_stats): CPU time of the render calls and of the last tick.
  uint64_t tick_usec_last_ = 0;
  uint64_t renders_ = 0, render_usec_total_ = 0, render_usec_last_ = 0, render_usec_max_ = 0;
  int64_t dirty_px_total_ = 0;
  godot::Ref<godot::CanvasItemMaterial> premul_;
  godot::RID canvas_item_;

  // Runtime callbacks run while JS is on the stack: they are queued in order
  // and emitted as signals once the runtime returned (flushMessages).
  struct Event {
    enum Kind { Message, State, Call, CallCancel, UiResult } kind;
    uint64_t id = 0;
    bool ok = false;
    bool null = false;  // State: the key was removed
    std::string name, json;
  };
  std::vector<Event> messages_;
  // Shared state set from Godot, re-applied when the runtime is recreated
  // (start() after stop(), bundle_url / dev_mode changes). Insertion order.
  std::vector<std::pair<std::string, std::string>> state_;
  void rememberState(const std::string& key);
  std::map<uint64_t, godot::Callable> ui_callbacks_;  // call_ui id -> callback
  std::string last_error_;
  std::string cursor_;
  struct HeldKey {
    std::string key, code;
  };
  std::map<int64_t, HeldKey> held_keys_;  // physical keycode -> what was sent on press
  struct Pad {
    bool trigger[2] = {false, false};
    bool buttons[17] = {};
  };
  std::map<int64_t, Pad> pads_;
};

}  // namespace jsui_godot
