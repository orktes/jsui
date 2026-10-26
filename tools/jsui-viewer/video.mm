// --video channel:texture=url (macOS): plays a video with AVFoundation into
// host texture `texture`, driven by the UI the way the showcase site's page
// drives its <video> (site/index.html trailerPlayer):
//
//   UI -> host   message `channel` { cmd: 'open' | 'close' | 'play' | 'pause' |
//                'mute' (muted) | 'seek' (to: s | fraction, play) }
//   host -> UI   HostState `channel` { status, muted, duration, backend, quality }
//                (status: idle | loading | playing | paused | ended | error),
//                buffer `channel.time` Float32 [time / duration, buffered /
//                duration, time in s], texture `texture` (opaque frames)
//
// With a Metal device (the Graphite renderer) each new frame is copied on the
// GPU into one texture the UI samples in place; otherwise its pixels are
// uploaded (jsui_set_texture_pixels). Every call is on the viewer's thread.
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cmath>
#include <string>
#include <vector>

#include "jsui/jsui.h"

struct ViewerVideo {
  JsuiRuntime* rt = nullptr;
  std::string channel, texture, timeKey, url;
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
  std::string quality = size.height > 0 ? std::to_string(int(size.height)) + "p H.264" : "";
  char d[32];
  std::snprintf(d, sizeof d, "%.3f", duration(v));
  std::string json = std::string("{\"status\":\"") + status(v) + "\",\"muted\":" + (v->muted ? "true" : "false") +
                     ",\"duration\":" + d + ",\"backend\":\"" + (v->device ? "metal" : "pixels") + "\"" +
                     (quality.empty() ? "" : ",\"quality\":" + jsonString(@(quality.c_str()))) + "}";
  if (json == v->lastState) return;
  v->lastState = json;
  jsui_patch_state(v->rt, v->channel.c_str(), json.c_str());
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

}  // namespace

extern "C" {

// `spec`: channel:texture=url. `device` / `queue`: the Graphite renderer's
// (NULL: upload pixels). NULL for a malformed spec.
ViewerVideo* viewer_video_create(const char* spec, void* device, void* queue) {
  std::string s = spec;
  size_t colon = s.find(':'), eq = s.find('=');
  if (colon == std::string::npos || eq == std::string::npos || colon == 0 || eq < colon + 2 || eq + 1 == s.size())
    return nullptr;
  auto* v = new ViewerVideo;
  v->channel = s.substr(0, colon);
  v->texture = s.substr(colon + 1, eq - colon - 1);
  v->timeKey = v->channel + ".time";
  v->url = s.substr(eq + 1);
  v->autoplay = !getenv("JSUI_VIDEO_NO_AUTOPLAY");
  if (device) {
    v->device = (__bridge id<MTLDevice>)device;
    v->queue = (__bridge id<MTLCommandQueue>)queue;
    CVMetalTextureCacheCreate(nullptr, nullptr, v->device, nullptr, &v->cache);
  }
  return v;
}

const char* viewer_video_channel(ViewerVideo* v) {
  return v->channel.c_str();
}

// A new runtime (reload): starts over, nothing open.
void viewer_video_attach(ViewerVideo* v, JsuiRuntime* rt) {
  if (v->player) [v->player pause];
  v->rt = rt;
  v->open = false;
  v->hasFrame = false;
  v->frame = nil;
  v->lastState.clear();
  v->lastTime.clear();
  publish(v);
}

// A message on the channel (its JSON payload).
void viewer_video_message(ViewerVideo* v, const char* json) {
  NSData* data = [NSData dataWithBytes:json length:std::strlen(json)];
  NSDictionary* m = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  if (![m isKindOfClass:[NSDictionary class]]) return;
  NSString* cmd = m[@"cmd"];
  if ([cmd isEqual:@"open"]) {
    v->open = true;
    if (v->autoplay) play(v);
  } else if ([cmd isEqual:@"close"]) {
    v->open = false;
    [v->player pause];
    v->hasFrame = false;
    jsui_remove_texture(v->rt, v->texture.c_str());
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

// Once per frame: the newest video frame, playback state and position.
// Returns 1 while frames are coming (keep ticking), else 0.
int viewer_video_tick(ViewerVideo* v) {
  if (!v->rt || !v->player || !v->open) return 0;
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

void viewer_video_destroy(ViewerVideo* v) {
  if (!v) return;
  [v->player pause];
  if (v->cache) CFRelease(v->cache);
  delete v;
}

}  // extern "C"
