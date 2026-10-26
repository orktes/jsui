// Video for the UI (macOS): the viewer as an example host that decodes video
// into host textures with AVFoundation. JSUI itself plays no video; the app
// asks its host for one with a host message, here the protocol the showcase
// site speaks with its page (site/index.html videoPlayer) and this viewer:
//
//   UI -> host   message 'video' { id, cmd: 'open' (texture, sources: [{ url,
//                type, height, quality }]: the first this host can play) |
//                'close' | 'play' | 'pause' | 'mute' (muted) | 'seek' (to: s |
//                fraction, play) }
//   host -> UI   HostState `id` { status, muted, duration, backend, quality }
//                (status: idle | loading | playing | paused | ended | error),
//                buffer `id.time` Float32 [time / duration, buffered /
//                duration, time in s], texture `texture` (opaque frames)
//
// With a Metal device (the Graphite renderer) each new frame is copied on the
// GPU into one texture the UI samples in place; otherwise its pixels are
// uploaded (jsui_set_texture_pixels). Every call is on the viewer's thread.
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "jsui/jsui.h"

struct ViewerVideo {
  JsuiRuntime* rt = nullptr;
  std::string name, texture, timeKey, url, quality;  // name: the message's id
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CVMetalTextureCacheRef cache = nullptr;
  id<MTLTexture> frame = nil;  // the texture the UI samples (Metal)
  AVPlayer* player = nil;
  AVPlayerItem* item = nil;
  AVPlayerItemVideoOutput* output = nil;
  bool open = false, autoplay = true, muted = true, hasFrame = false;
  std::string lastState, lastTime;
  std::vector<uint8_t> rgba;
};

namespace {

std::string jsonString(NSString* s) {
  NSData* d = [NSJSONSerialization dataWithJSONObject:@[ s ?: @"" ] options:0 error:nil];
  std::string a(static_cast<const char*>(d.bytes), d.length);
  return a.substr(1, a.size() - 2);  // ["..."] -> "..."
}

double seconds(CMTime t) {
  return CMTIME_IS_NUMERIC(t) ? CMTimeGetSeconds(t) : 0;
}

double duration(ViewerVideo* v) {
  double d = v->item ? seconds(v->item.duration) : 0;
  return std::isfinite(d) && d > 0 ? d : 0;
}

const char* status(ViewerVideo* v) {
  if (!v->player) return "idle";
  if (v->item.status == AVPlayerItemStatusFailed) return "error";
  double d = duration(v);
  if (v->player.timeControlStatus == AVPlayerTimeControlStatusPaused)
    return d > 0 && seconds(v->player.currentTime) >= d - 0.05 ? "ended" : "paused";
  if (v->player.timeControlStatus == AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate || !v->hasFrame)
    return "loading";
  return "playing";
}

void publish(ViewerVideo* v) {
  CGSize size = v->item ? v->item.presentationSize : CGSizeZero;
  std::string quality = !v->quality.empty() ? v->quality
                        : size.height > 0   ? std::to_string(int(size.height)) + "p"
                                            : "";
  char d[32];
  std::snprintf(d, sizeof d, "%.3f", duration(v));
  std::string json = std::string("{\"status\":\"") + status(v) + "\",\"muted\":" + (v->muted ? "true" : "false") +
                     ",\"duration\":" + d + ",\"backend\":\"" + (v->device ? "metal" : "pixels") + "\"" +
                     (quality.empty() ? "" : ",\"quality\":" + jsonString(@(quality.c_str()))) + "}";
  if (json == v->lastState) return;
  v->lastState = json;
  jsui_patch_state(v->rt, v->name.c_str(), json.c_str());
}

void create(ViewerVideo* v) {
  NSURL* url = [NSURL URLWithString:@(v->url.c_str())];
  if (!url.scheme) url = [NSURL fileURLWithPath:@(v->url.c_str())];
  v->item = [AVPlayerItem playerItemWithURL:url];
  NSDictionary* attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  v->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
  [v->item addOutput:v->output];
  v->player = [AVPlayer playerWithPlayerItem:v->item];
  v->player.muted = v->muted;
}

void play(ViewerVideo* v) {
  if (v->url.empty()) return;
  if (!v->player) create(v);
  if (std::strcmp(status(v), "ended") == 0) [v->player seekToTime:kCMTimeZero];
  [v->player play];
}

// A new frame: into the UI's texture.
void upload(ViewerVideo* v, CVPixelBufferRef pb) {
  int w = int(CVPixelBufferGetWidth(pb)), h = int(CVPixelBufferGetHeight(pb));
  if (v->device) {
    CVMetalTextureRef cv = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(nullptr, v->cache, pb, nullptr, MTLPixelFormatBGRA8Unorm, w, h, 0,
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
      v->frame = [v->device newTextureWithDescriptor:td];
    }
    // On the render's queue: ordered before the next frame samples it. The
    // pixel buffer stays alive until the copy ran.
    id<MTLCommandBuffer> cb = [v->queue commandBuffer];
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
      jsui_set_texture(v->rt, v->texture.c_str(), &t);
    } else {
      jsui_texture_updated(v->rt, v->texture.c_str());
    }
  } else {
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint8_t* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    v->rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      const uint8_t* s = src + y * stride;
      uint8_t* d = v->rgba.data() + size_t(y) * w * 4;
      for (int x = 0; x < w; x++, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
      }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    jsui_set_texture_pixels(v->rt, v->texture.c_str(), v->rgba.data(), w, h, 0, JSUI_TEXTURE_OPAQUE);
  }
  v->hasFrame = true;
}

// Picks the first source AVFoundation can play (`type`: a MIME type, codecs
// allowed); without a Metal device (frames uploaded as pixels) the smallest.
bool pick(ViewerVideo* v, NSArray* sources) {
  NSDictionary* best = nil;
  for (NSDictionary* src in sources) {
    if (![src isKindOfClass:[NSDictionary class]] || ![src[@"url"] isKindOfClass:[NSString class]]) continue;
    NSString* type = [src[@"type"] isKindOfClass:[NSString class]] ? src[@"type"] : nil;
    if (type && ![AVURLAsset isPlayableExtendedMIMEType:type]) continue;
    if (!best) best = src;
    else if (!v->device && [src[@"height"] doubleValue] < [best[@"height"] doubleValue]) best = src;
    if (v->device) break;
  }
  if (!best) return false;
  std::string url = [best[@"url"] UTF8String];
  if (url != v->url) {
    if (v->player) [v->player pause];
    v->player = nil;
    v->item = nil;
    v->output = nil;
    v->hasFrame = false;
    v->url = url;
  }
  v->quality = [best[@"quality"] isKindOfClass:[NSString class]] ? [best[@"quality"] UTF8String] : "";
  return true;
}

void message(ViewerVideo* v, NSDictionary* m) {
  NSString* cmd = m[@"cmd"];
  if ([cmd isEqual:@"open"]) {
    if ([m[@"texture"] isKindOfClass:[NSString class]]) v->texture = [m[@"texture"] UTF8String];
    v->open = true;
    if ([m[@"sources"] isKindOfClass:[NSArray class]] && !pick(v, m[@"sources"]))
      std::fprintf(stderr, "video %s: no source AVFoundation can play\n", v->name.c_str());
    if (v->autoplay) play(v);
  } else if ([cmd isEqual:@"close"]) {
    v->open = false;
    [v->player pause];
    v->hasFrame = false;
    if (!v->texture.empty()) jsui_remove_texture(v->rt, v->texture.c_str());
    v->frame = nil;
  } else if ([cmd isEqual:@"play"]) {
    play(v);
  } else if ([cmd isEqual:@"pause"]) {
    [v->player pause];
  } else if ([cmd isEqual:@"mute"]) {
    v->muted = [m[@"muted"] boolValue];
    v->player.muted = v->muted;
  } else if ([cmd isEqual:@"seek"]) {
    double d = duration(v), to = -1;
    if ([m[@"fraction"] isKindOfClass:[NSNumber class]]) to = [m[@"fraction"] doubleValue] * d;
    else if ([m[@"to"] isKindOfClass:[NSNumber class]]) to = [m[@"to"] doubleValue];
    if (to >= 0 && v->player) {
      if (d > 0) to = std::min(to, d - 0.1);
      [v->player seekToTime:CMTimeMakeWithSeconds(to, 600) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
    }
    if ([m[@"play"] boolValue] && (!v->player || v->player.timeControlStatus == AVPlayerTimeControlStatusPaused))
      play(v);
  }
  publish(v);
}

// The newest frame, playback state and position. True while frames come.
bool tick(ViewerVideo* v) {
  if (!v->rt || !v->player || !v->open || v->texture.empty()) return false;
  @autoreleasepool {
    CMTime now = [v->output itemTimeForHostTime:CACurrentMediaTime()];
    if ([v->output hasNewPixelBufferForItemTime:now]) {
      CVPixelBufferRef pb = [v->output copyPixelBufferForItemTime:now itemTimeForDisplay:nullptr];
      if (pb) {
        upload(v, pb);
        CVPixelBufferRelease(pb);
      }
    }
    publish(v);
    double d = duration(v), t = seconds(v->player.currentTime), b = 0;
    for (NSValue* r in v->item.loadedTimeRanges) {
      CMTimeRange range = r.CMTimeRangeValue;
      b = std::max(b, seconds(CMTimeRangeGetEnd(range)));
    }
    char key[64];
    std::snprintf(key, sizeof key, "%.2f %.1f %.3f", t, b, d);
    if (v->lastTime != key) {
      v->lastTime = key;
      float buf[3] = {float(d > 0 ? t / d : 0), float(d > 0 ? b / d : 0), float(t)};
      jsui_set_buffer(v->rt, v->timeKey.c_str(), JSUI_BUFFER_F32, buf, 3);
    }
    return v->player.timeControlStatus != AVPlayerTimeControlStatusPaused;
  }
}

}  // namespace

// Every video the UI opened, by id.
struct ViewerVideos {
  JsuiRuntime* rt = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  CVMetalTextureCacheRef cache = nullptr;
  std::map<std::string, ViewerVideo*> videos;
};

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

// A new runtime (reload): the videos of the old one stop.
void viewer_videos_attach(ViewerVideos* vs, JsuiRuntime* rt) {
  for (auto& [name, v] : vs->videos) {
    [v->player pause];
    delete v;
  }
  vs->videos.clear();
  vs->rt = rt;
}

// A 'video' message (its JSON payload).
void viewer_videos_message(ViewerVideos* vs, const char* json) {
  NSData* data = [NSData dataWithBytes:json length:std::strlen(json)];
  NSDictionary* m = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  if (![m isKindOfClass:[NSDictionary class]] || ![m[@"id"] isKindOfClass:[NSString class]]) return;
  std::string name = [m[@"id"] UTF8String];
  ViewerVideo*& v = vs->videos[name];
  if (!v) {
    v = new ViewerVideo;
    v->rt = vs->rt;
    v->name = name;
    v->timeKey = name + ".time";
    v->device = vs->device;
    v->queue = vs->queue;
    v->cache = vs->cache;
    v->autoplay = !getenv("JSUI_VIDEO_NO_AUTOPLAY");
  }
  message(v, m);
}

// Once per frame. Returns 1 while a video plays (keep ticking), else 0.
int viewer_videos_tick(ViewerVideos* vs) {
  bool playing = false;
  for (auto& [name, v] : vs->videos) playing |= tick(v);
  return playing;
}

void viewer_videos_destroy(ViewerVideos* vs) {
  if (!vs) return;
  viewer_videos_attach(vs, nullptr);
  if (vs->cache) CFRelease(vs->cache);
  delete vs;
}

}  // extern "C"
