// One root view of a runtime (React Native's RCTRootView / ReactSurface): a
// document with its own viewport, input, focus, cursor and render target
// bookkeeping. Every runtime has a main surface (root tag 1, the JsuiRuntime
// handle from jsui_create); jsui_surface_create adds more, each showing an
// AppRegistry component, all sharing the runtime's JS context, state, fonts
// and image cache.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/document.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkRegion.h"
#include "jsui/jsui.h"

class SkCanvas;
class SkPicture;

namespace jsui {

class Runtime;
class InputManager;
class DevOverlay;

class Surface {
 public:
  Surface(Runtime& rt, uint32_t* nextNodeId);
  ~Surface();

  Runtime& rt;
  Document doc;
  std::unique_ptr<InputManager> input;
  std::string cursor = "default";
  // The C handle of this surface (the runtime's own handle for the main one).
  JsuiRuntime* handle = nullptr;

  // AppRegistry key and initial props (JSON) this surface runs. An empty key
  // on the main surface: the bundle decides (AppRegistry auto-start, or an
  // explicit render()).
  std::string appKey;
  std::string propsJson = "{}";

  // The root tag (React Native): the id of the root container node.
  uint32_t tag() const { return doc.root()->id(); }

  // Sets the viewport and tells JS (a "resize" event with this rootTag).
  void setViewport(float width, float height, float pixelRatio);

  // Something new to paint. An approximate repaint (Document::
  // markExactRepaint) is redone exactly in the next frame: right away once
  // nothing drives frames any more (Runtime::animatingFrames), else in the
  // frame the animation renders anyway (still approximately while it moves
  // the node), so the surface still goes idle between animation frames.
  bool needsRedraw() const;
  void requestRedraw() { redraw_ = true; }

  void render(void* pixels, int width, int height, size_t rowBytes);
  // Like render(), but `pixels` must still hold the frame this surface last
  // rendered into it: only the changed region is repainted. Returns the
  // repainted device rect (empty when nothing changed). A different buffer,
  // size or an intervening render() makes it a full repaint.
  SkIRect renderIncremental(void* pixels, int width, int height, size_t rowBytes);
  // Full repaint into any canvas of width x height device px (the GPU
  // backend's surfaces). `clear`: clear to transparent first, else draw over
  // what the canvas holds. The next renderIncremental() repaints fully.
  void renderCanvas(SkCanvas* canvas, int width, int height, bool clear);
  // Damage-tracked repaint for a GPU backend that may keep the frame in a
  // surface of its own (the WebGPU backend's retained frame texture; its
  // targets, canvas textures, start cleared every frame). `key` names that
  // retained surface (nullptr: none yet); it is valid when it holds the frame
  // this surface last painted. Once the frame's damage is known (the bounds
  // of the changed device pixels), `choose` says where to paint:
  //   * {canvas, key}: `canvas` is the retained surface `key`. When it was
  //     valid only the damage bounds are repainted, clipped to that one rect
  //     (a scissor: complex clips cost GPU backends extra passes); otherwise
  //     the whole frame. The next frame can repaint incrementally again.
  //   * {canvas, nullptr}: a full repaint into some other canvas (the target
  //     itself, when most of the frame changed anyway); the retained surface
  //     misses this frame, so its next use repaints fully.
  //   * {nullptr}: nothing is painted (the backend failed).
  // Returns the repainted device rect.
  struct CanvasChoice {
    SkCanvas* canvas = nullptr;
    const void* key = nullptr;  // the retained surface painted into, or nullptr
    bool clear = true;          // full repaints: clear first, else draw over the canvas
    // Damage-only repaints: a few rects covering the damage, each painted
    // in its own pass scissored to it (see paintDocumentRects), instead of
    // the one bounding rect. Fewer pixels when the damage is scattered;
    // a pass costs a tree walk and the draws it crosses again.
    bool splitDamage = false;
  };
  // While `choose` runs: the frame's exact damage region (device pixels).
  const SkRegion& trackedDamage() const { return trackedDamage_; }
  // The rects the last damage-only renderCanvasTracked() repainted (each
  // cleared and painted whole; they cover trackedDamage() in a few rects).
  const std::vector<SkIRect>& trackedRects() const { return trackedRects_; }
  // `growCurves`: see updateDamage (false only for canvases whose curved
  // outlines do not depend on the damage clip).
  template <typename Choose>
  SkIRect renderCanvasTracked(const void* key, int width, int height, Choose&& choose, bool growCurves = true) {
    bool keyValid = false;
    SkIRect damage = beginCanvasTracked(key, width, height, &keyValid, growCurves);
    return finishCanvasTracked(choose(damage, keyValid), width, height, keyValid, damage);
  }

  // Dev mode reload: keeps the current frame on screen until the new bundle
  // renders (and while it is broken).
  void snapshotFrame();

 private:
  SkIRect renderImpl(void* pixels, int width, int height, size_t rowBytes, bool incremental);
  SkIRect paintTracked(SkCanvas* canvas, const void* key, int width, int height, size_t rowBytes,
                       bool incremental);
  // The full frame: document (or the frozen last good frame) + dev overlay.
  void paintFrame(SkCanvas* canvas, bool frozen, bool overlay);
  // Dev mode: show the frozen frame / the error overlay this frame.
  bool frozen() const;
  bool overlay() const;
  // renderCanvasTracked(): layout + damage (the bounds, or empty), then the paint.
  SkIRect beginCanvasTracked(const void* key, int width, int height, bool* keyValid, bool growCurves);
  SkIRect finishCanvasTracked(const CanvasChoice& c, int width, int height, bool keyValid, const SkIRect& damage);
  bool trackedFrozen_ = false, trackedOverlay_ = false;
  const void* trackedKey_ = nullptr;
  SkRegion trackedDamage_;
  std::vector<SkIRect> trackedRects_;
  // The buffer the last frame was painted into (incremental repaint).
  const void* lastTarget_ = nullptr;
  int lastTargetW_ = 0, lastTargetH_ = 0;
  size_t lastTargetRowBytes_ = 0;
  bool redraw_ = true;
  sk_sp<SkPicture> lastGoodFrame_;
  std::unique_ptr<DevOverlay> overlay_;
};

}  // namespace jsui
