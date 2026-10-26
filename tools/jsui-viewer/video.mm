// The viewer's video handler (JsuiConfig.on_video, include/jsui/jsui.h
// "Video"), macOS: each <Video> the UI opens plays with AVFoundation, its
// frames go to the view's host texture, and its status goes back with
// jsui_video_status once per frame (viewer_videos_tick). JSUI decodes no
// video: this is the viewer playing the part of a host, as a game engine
// would with its own player. With a Metal device (the Graphite renderer)
// each new frame is copied on the GPU into one texture the UI samples in
// place; otherwise its pixels are uploaded (jsui_set_texture_pixels). Every
// call is on the viewer's thread.
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "jsui/jsui.h"

struct ViewerVideos;

struct ViewerVideo {
  ViewerVideos* vs = nullptr;
  uint64_t video = 0;
  std::string texture;
  AVPlayer* player = nil;
  AVPlayerItem* item = nil;
  AVPlayerItemVideoOutput* output = nil;
  id<MTLTexture> frame = nil;  // the texture the UI samples (Metal)
  bool paused = true, loop = false, hasFrame = false;
  float rate = 1;
  std::string last;  // the last status reported
};

struct ViewerVideos {
  JsuiRuntime* rt = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CVMetalTextureCacheRef cache = nullptr;
  std::map<uint64_t, ViewerVideo*> videos;
  std::vector<uint8_t> rgba;
};

namespace {

double seconds(CMTime t) {
  double s = CMTIME_IS_NUMERIC(t) ? CMTimeGetSeconds(t) : 0;
  return std::isfinite(s) ? s : 0;
}

double duration(ViewerVideo* v) {
  double d = v->item ? seconds(v->item.duration) : 0;
  return d > 0 ? d : 0;
}

bool atEnd(ViewerVideo* v) {
  double d = duration(v);
  return d > 0 && seconds(v->player.currentTime) >= d - 0.05;
}

// The playback state the UI wants (JsuiVideoCommand OPEN / SET).
void apply(ViewerVideo* v, const JsuiVideoCommand* c) {
  v->player.muted = c->muted != 0;
  v->player.volume = std::max(0.f, std::min(1.f, c->volume));
  v->loop = c->loop != 0;
  v->rate = c->rate > 0 ? c->rate : 1;
  v->paused = c->paused != 0;
  if (v->paused) {
    [v->player pause];
    return;
  }
  if (atEnd(v)) [v->player seekToTime:kCMTimeZero];
  v->player.defaultRate = v->rate;
  [v->player playImmediatelyAtRate:v->rate];
}

void openVideo(ViewerVideos* vs, const JsuiVideoCommand* c) {
  // The first alternative AVFoundation can play (a type it does not know: skipped).
  const char* url = nullptr;
  for (int i = 0; i < c->source_count && !url; i++) {
    const char* type = c->source_types ? c->source_types[i] : nullptr;
    if (!type || [AVURLAsset isPlayableExtendedMIMEType:@(type)]) url = c->sources[i];
  }
  if (!url && c->source_count == 0) url = c->url;
  auto* v = new ViewerVideo;
  v->vs = vs;
  v->video = c->video;
  v->texture = c->texture ? c->texture : "";
  vs->videos[c->video] = v;
  if (!url) {
    JsuiVideoStatus s{};
    s.state = JSUI_VIDEO_FAILED;
    s.error = "no source AVFoundation can play";
    jsui_video_status(vs->rt, c->video, &s);
    return;
  }
  NSURL* u = [NSURL URLWithString:@(url)];
  if (!u.scheme) u = [NSURL fileURLWithPath:@(url)];
  v->item = [AVPlayerItem playerItemWithURL:u];
  NSDictionary* attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  v->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
  [v->item addOutput:v->output];
  v->player = [AVPlayer playerWithPlayerItem:v->item];
  v->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
  apply(v, c);
}

void closeVideo(ViewerVideos* vs, uint64_t video) {
  auto it = vs->videos.find(video);
  if (it == vs->videos.end()) return;
  [it->second->player pause];
  delete it->second;
  vs->videos.erase(it);
}

// A new frame: into the UI's texture.
void upload(ViewerVideo* v, CVPixelBufferRef pb) {
  int w = int(CVPixelBufferGetWidth(pb)), h = int(CVPixelBufferGetHeight(pb));
  if (v->vs->device) {
    CVMetalTextureRef cv = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(nullptr, v->vs->cache, pb, nullptr, MTLPixelFormatBGRA8Unorm, w, h, 0,
                                                  &cv) != kCVReturnSuccess)
      return;
    bool fresh = !v->frame || v->frame.width != NSUInteger(w) || v->frame.height != NSUInteger(h);
    if (fresh) {
      MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
      td.usage = MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModePrivate;
      v->frame = [v->vs->device newTextureWithDescriptor:td];
    }
    // On the render's queue: ordered before the next frame samples it. The
    // pixel buffer stays alive until the copy ran.
    id<MTLCommandBuffer> cb = [v->vs->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:CVMetalTextureGetTexture(cv) toTexture:v->frame];
    [blit endEncoding];
    CVPixelBufferRetain(pb);
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      CFRelease(cv);
      CVPixelBufferRelease(pb);
    }];
    [cb commit];
    if (fresh) {
      JsuiTexture t{};
      t.kind = JSUI_TEXTURE_METAL;
      t.handle = (__bridge void*)v->frame;
      t.format = MTLPixelFormatBGRA8Unorm;
      t.width = w;
      t.height = h;
      t.flags = JSUI_TEXTURE_OPAQUE;
      jsui_set_texture(v->vs->rt, v->texture.c_str(), &t);
    } else {
      jsui_texture_updated(v->vs->rt, v->texture.c_str());
    }
  } else {
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint8_t* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    v->vs->rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      const uint8_t* s = src + y * stride;
      uint8_t* d = v->vs->rgba.data() + size_t(y) * w * 4;
      for (int x = 0; x < w; x++, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
      }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    jsui_set_texture_pixels(v->vs->rt, v->texture.c_str(), v->vs->rgba.data(), w, h, 0, JSUI_TEXTURE_OPAQUE);
  }
  v->hasFrame = true;
}

// The newest frame, and the status when it changed. True while it plays.
bool tick(ViewerVideo* v) {
  if (!v->player || !v->vs->rt) return false;
  @autoreleasepool {
    CMTime now = [v->output itemTimeForHostTime:CACurrentMediaTime()];
    if (!v->texture.empty() && [v->output hasNewPixelBufferForItemTime:now]) {
      CVPixelBufferRef pb = [v->output copyPixelBufferForItemTime:now itemTimeForDisplay:nullptr];
      if (pb) {
        upload(v, pb);
        CVPixelBufferRelease(pb);
      }
    }
    // Looping: from the start again instead of ending.
    if (v->loop && !v->paused && v->player.timeControlStatus == AVPlayerTimeControlStatusPaused && atEnd(v)) {
      [v->player seekToTime:kCMTimeZero];
      [v->player playImmediatelyAtRate:v->rate];
    }
    JsuiVideoStatus s{};
    NSString* error = nil;
    if (v->item.status == AVPlayerItemStatusFailed) {
      s.state = JSUI_VIDEO_FAILED;
      error = v->item.error.localizedDescription ?: @"the video could not be played";
    } else if (v->item.status == AVPlayerItemStatusUnknown) {
      s.state = JSUI_VIDEO_LOADING;
    } else if (v->player.timeControlStatus == AVPlayerTimeControlStatusPlaying) {
      s.state = JSUI_VIDEO_PLAYING;
    } else if (v->player.timeControlStatus == AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate) {
      s.state = JSUI_VIDEO_BUFFERING;
    } else {
      s.state = !v->loop && atEnd(v) ? JSUI_VIDEO_ENDED : JSUI_VIDEO_PAUSED;
    }
    s.duration_s = duration(v);
    s.position_s = seconds(v->player.currentTime);
    for (NSValue* r in v->item.loadedTimeRanges) s.buffered_s = std::max(s.buffered_s, seconds(CMTimeRangeGetEnd(r.CMTimeRangeValue)));
    CGSize size = v->item.presentationSize;
    s.width = int(size.width);
    s.height = int(size.height);
    s.error = error.UTF8String;
    char key[160];
    std::snprintf(key, sizeof key, "%d %.3f %.3f %.1f %d %d", s.state, s.duration_s, s.position_s, s.buffered_s,
                  s.width, s.height);
    if (v->last != key) {
      v->last = key;
      jsui_video_status(v->vs->rt, v->video, &s);
    }
    return s.state == JSUI_VIDEO_PLAYING || s.state == JSUI_VIDEO_BUFFERING || s.state == JSUI_VIDEO_LOADING;
  }
}

}  // namespace

extern "C" {

// `device` / `queue`: the Graphite renderer's (NULL: upload pixels).
ViewerVideos* viewer_videos_create(void* device, void* queue) {
  auto* vs = new ViewerVideos;
  if (device) {
    vs->device = (__bridge id<MTLDevice>)device;
    vs->queue = (__bridge id<MTLCommandQueue>)queue;
    CVMetalTextureCacheCreate(nullptr, nullptr, vs->device, nullptr, &vs->cache);
  }
  return vs;
}

// The runtime the videos report to (its JsuiConfig.on_video is
// viewer_videos_command with `vs` as the user pointer).
void viewer_videos_attach(ViewerVideos* vs, JsuiRuntime* rt) {
  vs->rt = rt;
}

void viewer_videos_command(void* user, const JsuiVideoCommand* c) {
  auto* vs = static_cast<ViewerVideos*>(user);
  if (c->op == JSUI_VIDEO_OPEN) return openVideo(vs, c);
  auto it = vs->videos.find(c->video);
  if (it == vs->videos.end()) return;
  ViewerVideo* v = it->second;
  if (c->op == JSUI_VIDEO_CLOSE) closeVideo(vs, c->video);
  else if (!v->player) return;
  else if (c->op == JSUI_VIDEO_SET) apply(v, c);
  else if (c->op == JSUI_VIDEO_SEEK)
    [v->player seekToTime:CMTimeMakeWithSeconds(std::max(0.0, c->time_s), 600)
          toleranceBefore:kCMTimeZero
           toleranceAfter:kCMTimeZero];
}

// Once per frame. Returns 1 while a video plays (keep ticking), else 0.
int viewer_videos_tick(ViewerVideos* vs) {
  bool playing = false;
  for (auto& [id, v] : vs->videos) playing |= tick(v);
  return playing;
}

void viewer_videos_destroy(ViewerVideos* vs) {
  if (!vs) return;
  for (auto& [id, v] : vs->videos) {
    [v->player pause];
    delete v;
  }
  if (vs->cache) CFRelease(vs->cache);
  delete vs;
}

}  // extern "C"
