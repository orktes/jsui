// JsuiView's UI thread (jsui_view.h "UI thread"): the runtime view's frame
// on a thread of the runtime's own.
//
//   main thread (process)          UI thread                main thread
//   ------------------------------ ------------------------ ----------------------------------
//   viewport, host textures,       jsui_tick_ex             LOCKSTEP: frame_pre_draw (this frame)
//   sounds -> kickUiFrame          jsui_needs_redraw        PIPELINED: next process()
//   (input, set_buffer, ... came   jsui_record_gpu_frame    -> completeUiFrame: wait, replay each
//   before, from the game)         per view                 view's frame (jsui_render_gpu_frame,
//                                                           texture_copy), signals, sounds, loads
//
// The runtime is used by one thread at a time: whatever the main thread does
// with it while a frame is in flight first waits for the UI thread
// (RuntimeRef -> waitRender -> waitUi), and the UI thread only runs between
// kickUiFrame and that wait. The runtime's callbacks run on the UI thread
// during the tick: messages, state, calls and call results queue as before
// (messages_, read by the main thread after the wait), the loader and the
// audio handler queue for the main thread (FileAccess, AudioStreamPlayers),
// logs print from there (Godot's printing is thread safe). JS runs on its
// own thread (required: without one JS would run inside whichever thread
// calls into the runtime), whose batches the tick applies as on the main
// thread.
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>

#include "jsui_audio.h"
#include "jsui_ui_thread.h"
#include "jsui_view.h"

using namespace godot;

namespace jsui_godot {

namespace {

uint64_t usecNow() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int JsuiView::get_active_ui_thread() const {
  const JsuiView* rv = host_ ? host_ : this;
  return rv->rt_.unsynced() ? rv->ui_mode_ : UI_THREAD_OFF;
}

namespace {

// godot-aw3's WebGPUInterop with the frame-submit callback, else nullptr.
Object* frameSubmitInterop() {
  Engine* engine = Engine::get_singleton();
  Object* interop = engine->has_singleton("WebGPUInterop") ? engine->get_singleton("WebGPUInterop") : nullptr;
  return interop && interop->has_method("add_frame_submit_callback") ? interop : nullptr;
}

}  // namespace

bool JsuiView::onUiThread() const {
  const JsuiView* rv = host_ ? host_ : this;
  return rv->ui_worker_ && rv->ui_worker_->current();
}

void JsuiView::onLoadAny(void* user, JsuiRuntime* rt, uint64_t id, const char* url) {
  auto* self = static_cast<JsuiView*>(user);
  if (self->onUiThread()) {
    std::lock_guard<std::mutex> lock(self->ui_queue_m_);
    self->ui_loads_.push_back({rt, id, url ? url : ""});
    return;
  }
  onLoad(nullptr, rt, id, url);
}

void JsuiView::onAudioAny(void* user, const JsuiAudioCommand* cmd) {
  auto* self = static_cast<JsuiView*>(user);
  if (!cmd || !self->audio_) return;
  if (self->onUiThread()) {
    QueuedAudio q;
    q.cmd = *cmd;
    q.sound = cmd->sound ? cmd->sound : "";
    q.bus = cmd->bus ? cmd->bus : "";
    q.bus_null = cmd->bus == nullptr;
    std::lock_guard<std::mutex> lock(self->ui_queue_m_);
    self->ui_audio_.push_back(std::move(q));
    return;
  }
  ViewAudio::onAudio(self->audio_.get(), cmd);
}

bool JsuiView::uiFrameReady() {
  if (ui_mode_ == UI_THREAD_OFF || host_ || !rt_.unsynced() || !gpu_) return false;
  // Godot renders on the main thread (its default): the replay goes where
  // the device is. With a separate render thread the GPU frames are queued
  // there as before.
  RenderingServer* rs = RenderingServer::get_singleton();
  if (!rs || !rs->is_on_render_thread()) return false;
  std::vector<JsuiView*> views{this};
  views.insert(views.end(), surfaces_.begin(), surfaces_.end());
  for (JsuiView* v : views) {
    if (!showsOurRuntime(v) || !v->rt_.unsynced()) continue;
    if (v->auto_size_ || v->gpu_job_pending_) return false;
    if (!v->is_visible_in_tree()) continue;
    JsuiGpuTarget t;
    if (!v->uiTarget(&t)) return false;
  }
  return true;
}

void JsuiView::kickUiFrame() {
  if (!ui_worker_) ui_worker_ = std::make_unique<UiWorker>();
  UiFrameJob& job = ui_job_;
  job.now = clockMs();
  job.flags = sync_js_next_frame_ ? JSUI_TICK_SYNC_JS : 0;
  sync_js_next_frame_ = false;
  job.gpu = gpu_;
  job.views.clear();
  job.tick_usec = job.record_usec = 0;
  std::vector<JsuiView*> views{this};
  views.insert(views.end(), surfaces_.begin(), surfaces_.end());
  for (JsuiView* v : views) {
    if (!showsOurRuntime(v) || !v->rt_.unsynced()) continue;
    UiViewJob j;
    j.view = v;
    j.rt = v->rt_.unsynced();
    j.visible = v->is_visible_in_tree() && v->uiTarget(&j.target);
    j.src = v->gpu_cur_.src;
    if (j.visible) j.dst = v->dstTexture();
    j.pending = v->render_pending_;
    job.views.push_back(j);
  }
  if (ui_mode_ == UI_THREAD_LOCKSTEP && !pre_draw_connected_ && !submit_connected_) {
    // Where the frame is taken back: right before Godot submits its frame
    // when the driver offers that (godot-aw3's WebGPUInterop), else before
    // Godot starts drawing it.
    Object* interop = renderer_ == Renderer::WebGPU ? frameSubmitInterop() : nullptr;
    if (interop) {
      interop->call("add_frame_submit_callback", callable_mp(this, &JsuiView::onFrameSubmit));
      submit_interop_ = interop;
      submit_connected_ = true;
    } else {
      RenderingServer::get_singleton()->connect("frame_pre_draw", callable_mp(this, &JsuiView::onFramePreDraw));
      pre_draw_connected_ = true;
    }
  }
  JsuiRuntime* rt = rt_.unsynced();
  ui_job_pending_ = true;
  ui_job_replayed_ = false;
  ui_frames_++;
  ui_worker_->post([this, rt] {
    // The UI thread: nothing of the view but ui_job_ (and the runtime's
    // callbacks, which queue) until the main thread waits.
    UiFrameJob& f = ui_job_;
    const uint64_t t0 = usecNow();
    jsui_tick_ex(rt, f.now, f.flags);
    const uint64_t t1 = usecNow();
    for (UiViewJob& j : f.views) {
      if (!j.visible) continue;
      j.redraw = j.pending || jsui_needs_redraw(j.rt);
      if (j.redraw) j.rc = jsui_record_gpu_frame(j.rt, f.gpu, &j.target, &j.frame);
    }
    f.tick_usec = t1 - t0;
    f.record_usec = usecNow() - t1;
  });
}

void JsuiView::waitUi() {
  JsuiView* rv = host_ ? host_ : this;
  UiWorker* w = rv->ui_worker_.get();
  if (!w || !w->busy() || w->current()) return;
  const uint64_t t0 = usecNow();
  w->wait();
  rv->ui_waits_++;
  rv->ui_wait_usec_total_ += usecNow() - t0;
}

void JsuiView::onFramePreDraw() {
  if (ui_job_pending_) completeUiFrame();
}

void JsuiView::onFrameSubmit() {
  // Inside the driver's submit: only the replay (JSUI's own GPU work and
  // copies); signals and the rest wait for the next process.
  if (!ui_job_pending_ || ui_job_replayed_) return;
  waitUi();
  replayUiFrames(/*rawCopy=*/true);
}

void JsuiView::replayUiFrames(bool rawCopy) {
  ui_job_replayed_ = true;
  const uint64_t t0 = usecNow();
  for (UiViewJob& j : ui_job_.views) {
    JsuiView* v = j.view;
    if (!v || !j.frame) continue;
    JsuiGpuFrame* frame = j.frame;
    j.frame = nullptr;
    // Still the target it was recorded for (nothing resized it meanwhile).
    bool same = v->rt_.unsynced() == j.rt && v->gpu_cur_.src == j.src;
    if (same && !rawCopy) {
      JsuiGpuTarget now;
      same = v->uiTarget(&now) && now.texture == j.target.texture && now.width == j.target.width &&
             now.height == j.target.height;
    }
    if (!same || (rawCopy && !j.dst)) {
      jsui_discard_gpu_frame(j.rt, frame);
      v->render_pending_ = true;
      continue;
    }
    j.replayed = true;
    j.ok = rawCopy ? v->replayGpuFrame(frame, j.target, j.dst, &j.dirty) : v->replayGpuFrame(frame, j.target);
  }
  ui_replay_usec_total_ += usecNow() - t0;
}

void JsuiView::forgetUiView(JsuiView* view) {
  if (!ui_job_pending_) return;
  waitUi();
  for (UiViewJob& j : ui_job_.views) {
    if (j.view != view) continue;
    if (j.frame) jsui_discard_gpu_frame(j.rt, j.frame);
    j.frame = nullptr;
    j.view = nullptr;
  }
}

void JsuiView::stopUiThread() {
  if (ui_job_pending_) {
    waitUi();
    for (UiViewJob& j : ui_job_.views)
      if (j.frame) jsui_discard_gpu_frame(j.rt, j.frame);
    ui_job_.views.clear();
    ui_job_pending_ = false;
  }
  std::lock_guard<std::mutex> lock(ui_queue_m_);
  ui_audio_.clear();
  ui_loads_.clear();
}

void JsuiView::completeUiFrame() {
  if (!ui_job_pending_) return;
  waitUi();
  // The frames first (unless the frame-submit callback did): what the tick
  // queued may run the game's code (signals), which may stop the view or
  // resize it.
  if (!ui_job_replayed_) replayUiFrames(/*rawCopy=*/false);
  ui_job_pending_ = false;
  UiFrameJob& f = ui_job_;
  tick_usec_last_ = f.tick_usec;
  ui_tick_usec_total_ += f.tick_usec;
  ui_record_usec_total_ += f.record_usec;
  std::vector<UiViewJob> views;
  views.swap(f.views);
  for (UiViewJob& j : views) {
    JsuiView* v = j.view;
    if (!v || !j.visible) continue;
    if (j.rc < 0) {
      // The context cannot record: render on the main thread from now on.
      UtilityFunctions::push_warning("JsuiView: the GPU context cannot record frames; UI thread off");
      ui_mode_ = UI_THREAD_OFF;
      v->render_pending_ = true;
      continue;
    }
    if (!j.redraw) continue;
    if (j.rc == 0) {
      v->render_pending_ = false;  // nothing changed
      continue;
    }
    if (!j.replayed) continue;  // discarded: render_pending_ is set
    if (!j.ok) {
      v->gpuFailed();
      v->render_pending_ = true;
      continue;
    }
    v->render_pending_ = false;
    if (j.dirty.width > 0) v->finishGpuFrame(j.dirty);  // frame-submit replays: frame_rendered now
  }
  // Sounds and loads the tick asked for.
  std::vector<QueuedAudio> audio;
  std::vector<QueuedLoad> loads;
  {
    std::lock_guard<std::mutex> lock(ui_queue_m_);
    audio.swap(ui_audio_);
    loads.swap(ui_loads_);
  }
  for (QueuedAudio& q : audio) {
    if (!audio_) break;
    q.cmd.sound = q.sound.c_str();
    q.cmd.bus = q.bus_null ? nullptr : q.bus.c_str();
    ViewAudio::onAudio(audio_.get(), &q.cmd);
  }
  for (QueuedLoad& l : loads)
    if (rt_.unsynced()) onLoad(nullptr, l.rt, l.id, l.url.c_str());
  // Then what the tick did, as process() does after a tick on the main thread.
  flushMessages();
  if (!rt_) return;  // a handler stopped the view
  reportErrors();
  std::vector<JsuiView*> all{this};
  all.insert(all.end(), surfaces_.begin(), surfaces_.end());
  for (JsuiView* v : all)
    if (showsOurRuntime(v)) v->afterTick();
}

}  // namespace jsui_godot
