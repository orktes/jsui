#include "render/container_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "core/document.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "platform/loader.h"
#include "render/painter.h"
#include "runtime.h"
#include "surface.h"

namespace jsui {

namespace container {
namespace {

NodeState* stateOf(const Node* node) {
  if (!node || node->type() != NodeType::Container) return nullptr;
  return static_cast<NodeState*>(node->imageCache.get());
}

void push(NodeState* st, Value::Object ev) {
  if (!st->cache || !st->node) return;
  st->cache->events.push_back({st->node->id(), Value(std::move(ev))});
}

Value parsed(const char* json) {
  if (!json) return Value();
  std::string err;
  Value v = Value::parseJson(json, &err);
  return err.empty() ? v : Value();
}

// The child's embedder callbacks: everything becomes an event for the
// parent's JS (delivered after the children's ticks), or goes to the parent's
// loader / log.
void onLog(void* user, JsuiLogLevel level, const char* message) {
  auto* st = static_cast<NodeState*>(user);
  std::string msg = message ? message : "";
  st->cache->runtime().log(level, "[JSUIContainer] " + msg);
  if (level == JSUI_LOG_ERROR) push(st, {{"kind", Value("error")}, {"message", Value(msg)}});
}

// The child's loads, whole (method, headers, body: fetch()), through the
// parent's loader; the answer goes back whole too (status text, headers).
void loadInherit(NodeState* st, ResourceLoader* childLoader, uint64_t requestId, LoadRequest req) {
  ResourceLoader& loader = st->cache->runtime().loader;
  uint64_t id = loader.reserveId();
  st->loads.insert(id);
  loader.load(id, std::move(req), [st, childLoader, requestId, id](Resource r) {
    st->loads.erase(id);
    childLoader->complete(requestId, std::move(r));
  });
}

Resource blocked(const std::string& url) {
  Resource r;
  r.status = 0;
  r.url = url;
  r.error = "blocked by the JSUIContainer's services";
  return r;
}

Value headersValue(const HeaderList& headers) {
  Value::Object o;
  for (auto& [k, v] : headers) o[k] = Value(v);
  return Value(std::move(o));
}

HeaderList headerList(const Value& v) {
  HeaderList out;
  for (auto& [k, val] : v.asObject()) out.emplace_back(k, val.isString() ? val.asString() : val.toJson());
  return out;
}

// The child's loads: the native rules, then the parent's loader or JS.
void forwardLoad(NodeState* st, ResourceLoader* childLoader, uint64_t requestId, LoadRequest req) {
  const Services& sv = st->services;
  if (sv.load == Services::None || !sv.admit(req.url)) {
    childLoader->complete(requestId, blocked(req.url));
    return;
  }
  if (sv.load == Services::Js) {
    Value::Object r{{"url", Value(req.url)}, {"method", Value(req.method)}, {"headers", headersValue(req.headers)}};
    if (!req.body.empty()) r.emplace("body", Value(std::string(req.body.begin(), req.body.end())));
    push(st, {{"kind", Value("load")},
              {"gen", Value(double(st->generation))},
              {"id", Value(double(requestId))},
              {"request", Value(std::move(r))}});
    st->jsLoads.emplace(requestId, std::move(req));
    return;
  }
  loadInherit(st, childLoader, requestId, std::move(req));
}

// load-next / load-reply / load-block from the parent's JS.
void loadAnswer(NodeState* st, const std::string& op, const Value& cmd) {
  auto it = st->jsLoads.find(uint64_t(cmd["id"].asNumber()));
  if (it == st->jsLoads.end() || !st->child) return;
  LoadRequest req = std::move(it->second);
  st->jsLoads.erase(it);
  ResourceLoader* childLoader = &internalRuntime(st->child)->loader;
  uint64_t requestId = uint64_t(cmd["id"].asNumber());
  if (op == "load-next") {
    const Value& r = cmd["request"];
    if (r.has("url")) req.url = ResourceLoader::resolve(st->baseUrl, r["url"].asString());
    if (r.has("method")) req.method = r["method"].asString();
    if (r.has("headers")) req.headers = headerList(r["headers"]);
    if (r.has("body")) {
      const std::string& b = r["body"].asString();
      req.body.assign(b.begin(), b.end());
    }
    // A rewritten URL still has to pass the rules.
    if (!st->services.admit(req.url)) childLoader->complete(requestId, blocked(req.url));
    else loadInherit(st, childLoader, requestId, std::move(req));
  } else if (op == "load-reply") {
    Resource res;
    res.url = req.url;
    res.status = int(cmd["status"].asNumber(200));
    res.statusText = cmd["statusText"].asString();
    res.headers = headerList(cmd["headers"]);
    for (auto& [k, v] : res.headers) {
      std::string lower = k;
      for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      if (lower == "content-type") res.contentType = v;
    }
    const std::string& body = cmd["body"].asString();
    res.data.assign(body.begin(), body.end());
    childLoader->complete(requestId, std::move(res));
  } else {
    childLoader->complete(requestId, blocked(req.url));
  }
}

void onMessage(void* user, const char* name, const char* json) {
  push(static_cast<NodeState*>(user),
       {{"kind", Value("message")}, {"name", Value(name ? name : "")}, {"payload", parsed(json)}});
}

void onState(void* user, const char* key, const char* json) {
  Value::Object ev{{"kind", Value("state")}, {"key", Value(key ? key : "")}, {"value", parsed(json)}};
  if (!json) ev.emplace("removed", Value(true));
  push(static_cast<NodeState*>(user), std::move(ev));
}

void onCall(void* user, uint64_t callId, const char* name, const char* argsJson) {
  push(static_cast<NodeState*>(user), {{"kind", Value("call")},
                                       {"callId", Value(double(callId))},
                                       {"name", Value(name ? name : "")},
                                       {"args", parsed(argsJson)}});
}

void onCallCancel(void* user, uint64_t callId) {
  push(static_cast<NodeState*>(user), {{"kind", Value("callCancel")}, {"callId", Value(double(callId))}});
}

// ---- audio and video: the child's requests play through the parent's
// Audio / Video (its handler, its built-in backend), so the embedder sees one
// runtime. Ids are the parent's own; reports go back to the child under its
// ids. A callback left in the parent after the child is gone (or replaced)
// does nothing.

// The child that `gen` named, if it is still running.
JsuiRuntime* alive(const std::shared_ptr<NodeState*>& life, uint64_t gen) {
  NodeState* st = *life;
  return st && st->generation == gen ? st->child : nullptr;
}

float busGain(NodeState* st, const std::string& bus) {
  auto it = st->busGains.find(bus);
  return it == st->busGains.end() ? 1.f : it->second;
}

void audioInherit(NodeState* st, const JsuiAudioCommand* c) {
  Audio& audio = *st->cache->runtime().audio;
  std::string bus = c->bus ? c->bus : "";
  switch (c->op) {
    case JSUI_AUDIO_PRELOAD: {
      std::string url = c->sound ? c->sound : "";
      audio.preloadFor(url, [life = st->life, gen = st->generation, url](bool ok, double d) {
        if (JsuiRuntime* child = alive(life, gen)) jsui_audio_sound_loaded(child, url.c_str(), ok, d);
      });
      break;
    }
    case JSUI_AUDIO_UNLOAD:  // the parent may still use it
      break;
    case JSUI_AUDIO_PLAY: {
      uint64_t voice = c->voice;
      uint64_t pv = audio.playFor(
          [life = st->life, gen = st->generation, voice](int status) {
            JsuiRuntime* child = alive(life, gen);
            if (!child) return;
            (*life)->voices.erase(voice);
            jsui_audio_voice_ended(child, voice, status);
          },
          c->sound ? c->sound : "", bus, c->volume * busGain(st, bus), c->pitch, c->pan, c->loop != 0, c->fade_ms,
          c->offset_s);
      // Failed at once (no audio): ended already, nothing to track.
      if (audio.live(pv)) st->voices[voice] = {pv, bus, c->volume, c->pitch, c->pan};
      break;
    }
    case JSUI_AUDIO_STOP:
    case JSUI_AUDIO_PAUSE:
    case JSUI_AUDIO_RESUME:
    case JSUI_AUDIO_SET: {
      auto it = st->voices.find(c->voice);
      if (it == st->voices.end()) break;
      NodeState::Voice& v = it->second;
      if (c->op == JSUI_AUDIO_SET) {
        v.volume = c->volume;
        v.pitch = c->pitch;
        v.pan = c->pan;
      }
      audio.voice(c->op, v.parent, v.volume * busGain(st, v.bus), v.pitch, v.pan, c->fade_ms);
      break;
    }
    case JSUI_AUDIO_BUS: {
      st->busGains[bus] = c->muted ? 0.f : c->volume;
      for (auto& [id, v] : st->voices)
        if (v.bus == bus)
          audio.voice(JSUI_AUDIO_SET, v.parent, v.volume * busGain(st, bus), v.pitch, v.pan, c->fade_ms);
      break;
    }
    case JSUI_AUDIO_STOP_ALL: {
      // Only the child's own voices.
      std::vector<uint64_t> stop;
      for (auto& [id, v] : st->voices)
        if (!c->bus || v.bus == bus) stop.push_back(v.parent);
      for (uint64_t pv : stop) audio.voice(JSUI_AUDIO_STOP, pv, 0, 1, 0, c->fade_ms);
      break;
    }
  }
}

// A request the services refused: what the child waits for fails.
void audioBlock(NodeState* st, const JsuiAudioCommand* c) {
  if (!st->child) return;
  if (c->op == JSUI_AUDIO_PLAY) jsui_audio_voice_ended(st->child, c->voice, JSUI_AUDIO_FAILED);
  else if (c->op == JSUI_AUDIO_PRELOAD) jsui_audio_sound_loaded(st->child, c->sound ? c->sound : "", 0, -1);
}

const char* const kAudioOps[] = {"preload", "unload", "play", "stop", "pause", "resume", "set", "bus", "stopAll"};

Value audioValue(const JsuiAudioCommand* c) {
  Value::Object o{{"op", Value(kAudioOps[c->op])}};
  switch (c->op) {
    case JSUI_AUDIO_PRELOAD:
    case JSUI_AUDIO_UNLOAD:
      o.emplace("sound", Value(c->sound ? c->sound : ""));
      break;
    case JSUI_AUDIO_PLAY:
      o.emplace("sound", Value(c->sound ? c->sound : ""));
      o.emplace("bus", Value(c->bus ? c->bus : ""));
      o.emplace("loop", Value(c->loop != 0));
      o.emplace("offset", Value(c->offset_s));
      [[fallthrough]];
    case JSUI_AUDIO_SET:
      o.emplace("volume", Value(double(c->volume)));
      o.emplace("pitch", Value(double(c->pitch)));
      o.emplace("pan", Value(double(c->pan)));
      [[fallthrough]];
    case JSUI_AUDIO_STOP:
    case JSUI_AUDIO_PAUSE:
    case JSUI_AUDIO_RESUME:
      o.emplace("voice", Value(double(c->voice)));
      o.emplace("fadeMs", Value(double(c->fade_ms)));
      break;
    case JSUI_AUDIO_BUS:
      o.emplace("bus", Value(c->bus ? c->bus : ""));
      o.emplace("volume", Value(double(c->volume)));
      o.emplace("muted", Value(c->muted != 0));
      o.emplace("fadeMs", Value(double(c->fade_ms)));
      break;
    case JSUI_AUDIO_STOP_ALL:
      o.emplace("bus", c->bus ? Value(c->bus) : Value());
      o.emplace("fadeMs", Value(double(c->fade_ms)));
      break;
  }
  return Value(std::move(o));
}

// A command back from the parent's JS (audio-next / audio-block): `orig` is
// what the child sent, `cmd` what the middleware passed on (its changes win;
// the op and voice are the child's).
template <typename Fn>
void withAudio(const Value& orig, const Value& cmd, Fn fn) {
  auto field = [&](const char* k) -> const Value& { return cmd.has(k) ? cmd[k] : orig[k]; };
  JsuiAudioCommand c{};
  std::string op = orig["op"].asString();
  for (int i = 0; i < int(std::size(kAudioOps)); i++)
    if (op == kAudioOps[i]) c.op = JsuiAudioOp(i);
  std::string sound = field("sound").asString(), bus = field("bus").asString();
  c.voice = uint64_t(orig["voice"].asNumber());
  c.sound = sound.c_str();
  c.bus = field("bus").isNull() && c.op == JSUI_AUDIO_STOP_ALL ? nullptr : bus.c_str();
  c.volume = float(field("volume").asNumber(1));
  c.pitch = float(field("pitch").asNumber(1));
  c.pan = float(field("pan").asNumber(0));
  c.fade_ms = float(field("fadeMs").asNumber(0));
  c.offset_s = field("offset").asNumber(0);
  c.loop = field("loop").asBool() ? 1 : 0;
  c.muted = field("muted").asBool() ? 1 : 0;
  fn(&c);
}

void onAudio(void* user, const JsuiAudioCommand* c) {
  auto* st = static_cast<NodeState*>(user);
  switch (st->services.audio) {
    case Services::Inherit:
      audioInherit(st, c);
      break;
    case Services::None:
      audioBlock(st, c);
      break;
    case Services::Js:
      push(st, {{"kind", Value("audio")}, {"gen", Value(double(st->generation))}, {"cmd", audioValue(c)}});
      break;
  }
}

std::string videoTexture(NodeState* st, uint64_t video) {
  return "jsuic:" + std::to_string(st->node->id()) + "/" + std::to_string(st->generation) + "/" +
         std::to_string(video);
}

void videoInherit(NodeState* st, const JsuiVideoCommand* c) {
  Runtime& parent = st->cache->runtime();
  Runtime* child = st->child ? internalRuntime(st->child) : nullptr;  // null: going away
  switch (c->op) {
    case JSUI_VIDEO_OPEN: {
      if (st->videos.count(c->video)) break;
      std::vector<std::string> sources, types;
      for (int i = 0; i < c->source_count; i++) {
        sources.push_back(c->sources[i] ? c->sources[i] : "");
        types.push_back(c->source_types && c->source_types[i] ? c->source_types[i] : "");
      }
      if (sources.empty() && c->url) sources.push_back(c->url);
      std::string texture = c->texture ? c->texture : "";
      std::string pt = videoTexture(st, c->video);
      uint64_t pv = parent.video->reserve();
      st->videos[c->video] = {pv, texture};
      // The child's view shows the parent's texture the host draws into.
      if (child && !texture.empty()) child->textures.alias(texture, pt);
      uint64_t video = c->video;
      parent.video->openFor(
          [life = st->life, gen = st->generation, video](const JsuiVideoStatus& s) {
            if (JsuiRuntime* ch = alive(life, gen)) jsui_video_status(ch, video, &s);
          },
          pv, sources, types, pt, c->paused != 0, c->muted != 0, c->volume, c->loop != 0, c->rate);
      break;
    }
    case JSUI_VIDEO_SET: {
      auto it = st->videos.find(c->video);
      if (it != st->videos.end())
        parent.video->set(it->second.parent, c->paused != 0, c->muted != 0, c->volume, c->loop != 0, c->rate);
      break;
    }
    case JSUI_VIDEO_SEEK: {
      auto it = st->videos.find(c->video);
      if (it != st->videos.end()) parent.video->seek(it->second.parent, c->time_s);
      break;
    }
    case JSUI_VIDEO_CLOSE: {
      auto it = st->videos.find(c->video);
      if (it == st->videos.end()) break;
      if (child && !it->second.texture.empty()) child->textures.alias(it->second.texture, "");
      parent.video->close(it->second.parent);
      st->videos.erase(it);
      break;
    }
  }
}

void videoBlock(NodeState* st, const JsuiVideoCommand* c) {
  if (!st->child || c->op != JSUI_VIDEO_OPEN) return;
  JsuiVideoStatus s{};
  s.state = JSUI_VIDEO_FAILED;
  s.error = "blocked by the JSUIContainer's services";
  jsui_video_status(st->child, c->video, &s);
}

const char* const kVideoOps[] = {"open", "set", "seek", "close"};

Value videoValue(const JsuiVideoCommand* c) {
  Value::Object o{{"op", Value(kVideoOps[c->op])}, {"video", Value(double(c->video))}};
  if (c->op == JSUI_VIDEO_OPEN) {
    Value::Array sources;
    for (int i = 0; i < c->source_count; i++) {
      Value::Object src{{"uri", Value(c->sources[i] ? c->sources[i] : "")}};
      if (c->source_types && c->source_types[i]) src.emplace("type", Value(c->source_types[i]));
      sources.push_back(Value(std::move(src)));
    }
    o.emplace("sources", Value(std::move(sources)));
    o.emplace("texture", Value(c->texture ? c->texture : ""));
  }
  if (c->op == JSUI_VIDEO_OPEN || c->op == JSUI_VIDEO_SET) {
    o.emplace("paused", Value(c->paused != 0));
    o.emplace("muted", Value(c->muted != 0));
    o.emplace("volume", Value(double(c->volume)));
    o.emplace("loop", Value(c->loop != 0));
    o.emplace("rate", Value(double(c->rate)));
  }
  if (c->op == JSUI_VIDEO_SEEK) o.emplace("time", Value(c->time_s));
  return Value(std::move(o));
}

template <typename Fn>
void withVideo(const Value& orig, const Value& cmd, Fn fn) {
  auto field = [&](const char* k) -> const Value& { return cmd.has(k) ? cmd[k] : orig[k]; };
  JsuiVideoCommand c{};
  std::string op = orig["op"].asString();
  for (int i = 0; i < int(std::size(kVideoOps)); i++)
    if (op == kVideoOps[i]) c.op = JsuiVideoOp(i);
  c.video = uint64_t(orig["video"].asNumber());
  std::vector<std::string> urls, types;
  for (const Value& src : field("sources").asArray()) {
    urls.push_back(src.isString() ? src.asString() : src["uri"].asString());
    types.push_back(src["type"].asString());
  }
  std::vector<const char*> urlPtrs, typePtrs;
  for (size_t i = 0; i < urls.size(); i++) {
    urlPtrs.push_back(urls[i].c_str());
    typePtrs.push_back(types[i].empty() ? nullptr : types[i].c_str());
  }
  std::string texture = orig["texture"].asString();  // the child's key, always
  c.url = urlPtrs.empty() ? nullptr : urlPtrs[0];
  c.sources = urlPtrs.data();
  c.source_types = typePtrs.data();
  c.source_count = int(urlPtrs.size());
  c.texture = texture.c_str();
  c.paused = field("paused").asBool() ? 1 : 0;
  c.muted = field("muted").asBool() ? 1 : 0;
  c.volume = float(field("volume").asNumber(1));
  c.loop = field("loop").asBool() ? 1 : 0;
  c.rate = float(field("rate").asNumber(1));
  c.time_s = field("time").asNumber(0);
  fn(&c);
}

void onVideo(void* user, const JsuiVideoCommand* c) {
  auto* st = static_cast<NodeState*>(user);
  switch (st->services.video) {
    case Services::Inherit:
      videoInherit(st, c);
      break;
    case Services::None:
      videoBlock(st, c);
      break;
    case Services::Js:
      push(st, {{"kind", Value("video")}, {"gen", Value(double(st->generation))}, {"cmd", videoValue(c)}});
      break;
  }
}

// What the child still plays through the parent stops with it.
void releaseMedia(NodeState* st) {
  Runtime& parent = st->cache->runtime();
  for (auto& [id, v] : st->voices) parent.audio->voice(JSUI_AUDIO_STOP, v.parent, 0, 1, 0, 0);
  st->voices.clear();
  st->busGains.clear();
  for (auto& [id, v] : st->videos) parent.video->close(v.parent);
  st->videos.clear();
}

// A parent -> child call (jsui_call_ui) on its way back.
struct UiCall {
  Containers* cache;
  uint32_t node;
  double seq;
};

void uiCallDone(void* user, uint64_t, int ok, const char* result) {
  auto* c = static_cast<UiCall*>(user);
  Value::Object ev{{"kind", Value("callResult")}, {"seq", Value(c->seq)}, {"ok", Value(ok != 0)}};
  if (ok) ev.emplace("result", parsed(result));
  else ev.emplace("error", Value(result ? result : "failed"));
  c->cache->events.push_back({c->node, Value(std::move(ev))});
  delete c;
}

// The embedder's JsuiConfig.on_child (0: refused).
int childEvent(NodeState* st, JsuiChildEvent event, JsuiRuntime* child, JsuiConfig* cfg) {
  Runtime& parent = st->cache->runtime();
  if (!parent.config.on_child) return 1;
  std::string nativeId = st->node->props()["nativeID"].asString();
  JsuiChildInfo info{};
  info.event = event;
  info.child = child;
  info.config = cfg;
  info.source_url = st->data.empty() && !st->src.empty() ? st->src.c_str() : nullptr;
  info.node = st->node->id();
  info.native_id = nativeId.c_str();
  return parent.config.on_child(parent.config.child_user, parent.handle, &info);
}

// The services that are part of the child runtime itself: fonts, textures.
void applyInheritance(NodeState* st) {
  if (!st->child) return;
  Runtime& parent = st->cache->runtime();
  Runtime* childRt = internalRuntime(st->child);
  FontManager* fonts = st->services.fonts ? &parent.fonts : nullptr;
  if (childRt->fonts.inherited() != fonts) {
    childRt->fonts.inherit(fonts);
    childRt->fontsChanged();
  }
  HostTextures& tex = childRt->textures;
  // Re-registered under the new rules (inherit() moves the views' uses).
  tex.inherit(nullptr);
  tex.inheritAll = st->services.textures;
  for (const auto& [key, pk] : st->appliedAliases)
    if (!st->services.textureAliases.count(key)) tex.alias(key, "");
  for (const auto& [key, pk] : st->services.textureAliases) tex.alias(key, pk);
  st->appliedAliases = st->services.textureAliases;
  tex.inherit(&parent.textures);
}

void destroyChild(NodeState* st) {
  if (!st->child) return;
  ResourceLoader& loader = st->cache->runtime().loader;
  for (uint64_t id : st->loads) loader.cancel(id);
  st->loads.clear();
  st->jsLoads.clear();
  JsuiRuntime* child = st->child;
  childEvent(st, JSUI_CHILD_DESTROYED, child, nullptr);
  st->child = nullptr;
  jsui_destroy(child);  // fails the child's unsettled calls (uiCallDone)
  releaseMedia(st);     // its voices and videos in the parent
}

void createChild(NodeState* st) {
  destroyChild(st);
  if (st->src.empty() && st->data.empty()) return;
  Runtime& parent = st->cache->runtime();
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = std::max(1, int(std::ceil(st->width)));
  cfg.height = std::max(1, int(std::ceil(st->height)));
  cfg.pixel_ratio = st->pixelRatio > 0 ? st->pixelRatio : 1;
  st->baseUrl = st->src.empty() ? parent.baseUrl : st->src;
  cfg.base_url = st->baseUrl.empty() ? nullptr : st->baseUrl.c_str();
  cfg.log = onLog;
  cfg.log_user = st;
  cfg.on_message = onMessage;
  cfg.message_user = st;
  cfg.on_state = onState;
  cfg.state_user = st;
  cfg.on_call = onCall;
  cfg.on_call_cancel = onCallCancel;
  cfg.call_user = st;
  cfg.js_memory_limit_mb = parent.config.js_memory_limit_mb;
  cfg.js_timeout_ms = parent.config.js_timeout_ms;
  cfg.audio_disabled = parent.config.audio_disabled;
  // Audio and video play through the parent's (see onAudio / onVideo).
  cfg.on_audio = onAudio;
  cfg.audio_user = st;
  cfg.on_video = onVideo;
  cfg.video_user = st;
  // The embedder's say (its own handlers, or no child at all).
  if (!childEvent(st, JSUI_CHILD_CONFIGURE, nullptr, &cfg)) {
    push(st, {{"kind", Value("error")}, {"message", Value("the host refused this JSUIContainer")}});
    return;
  }
  st->generation++;
  st->appliedAliases.clear();
  st->child = jsui_create(&cfg);
  Runtime* childRt = internalRuntime(st->child);
  // The parent's fonts and host textures are the child's too, under its own.
  applyInheritance(st);
  st->parentFonts = parent.fonts.generation;
  // Loads go through the parent, unless the embedder gave the child a loader.
  if (!cfg.load) {
    ResourceLoader* childLoader = &childRt->loader;
    childLoader->forward = [st, childLoader](uint64_t id, LoadRequest req) {
      forwardLoad(st, childLoader, id, std::move(req));
    };
  }
  childEvent(st, JSUI_CHILD_CREATED, st->child, nullptr);
  // The exact (fractional) viewport.
  internalRuntime(st->child)->main().setViewport(std::max(0.f, st->width), std::max(0.f, st->height),
                                                 cfg.pixel_ratio);
  if (!st->appKey.empty()) jsui_surface_run(st->child, st->appKey.c_str(), st->propsJson.c_str());
  st->runPending = false;
  if (!st->data.empty())
    jsui_load_bundle_source(st->child, st->data.data(), st->data.size(), "JSUIContainer");
  else
    jsui_load_bundle_url(st->child, st->src.c_str());
}

std::string stringOf(const Value& v) { return v.isString() ? v.asString() : v.isNull() ? "null" : v.toJson(); }

void runCommand(NodeState* st, const Value& cmd) {
  JsuiRuntime* child = st->child;
  const std::string& op = cmd["op"].asString();
  if (op == "reload") {
    createChild(st);
    return;
  }
  if (op == "call") {
    auto* c = new UiCall{st->cache, st->node->id(), cmd["seq"].asNumber()};
    if (!child) {
      uiCallDone(c, 0, 0, "the container has no running bundle");
      return;
    }
    std::string args = cmd["args"].toJson();
    jsui_call_ui(child, cmd["name"].asString().c_str(), args.c_str(), uiCallDone, c);
    return;
  }
  if (!child) return;
  // Answers for an earlier child runtime (reloaded since) are dropped.
  bool answer = op.compare(0, 5, "load-") == 0 || op.compare(0, 6, "audio-") == 0 || op.compare(0, 6, "video-") == 0;
  if (answer && uint64_t(cmd["gen"].asNumber()) != st->generation) return;
  if (op == "load-next" || op == "load-reply" || op == "load-block") {
    loadAnswer(st, op, cmd);
    return;
  }
  if (op == "audio-next" || op == "audio-block") {
    withAudio(cmd["orig"], cmd["cmd"], [&](const JsuiAudioCommand* c) {
      if (op == "audio-next") audioInherit(st, c);
      else audioBlock(st, c);
    });
    return;
  }
  if (op == "video-next" || op == "video-block") {
    withVideo(cmd["orig"], cmd["cmd"], [&](const JsuiVideoCommand* c) {
      if (op == "video-next") videoInherit(st, c);
      else videoBlock(st, c);
    });
    return;
  }
  if (op == "post") {
    std::string json = cmd["payload"].toJson();
    jsui_post_message(child, cmd["name"].asString().c_str(), json.c_str());
  } else if (op == "resolve") {
    std::string json = cmd["value"].toJson();
    jsui_resolve_call(child, uint64_t(cmd["callId"].asNumber()), json.c_str());
  } else if (op == "reject") {
    jsui_reject_call(child, uint64_t(cmd["callId"].asNumber()), cmd["message"].asString().c_str());
  } else if (op == "state") {
    const std::string& key = cmd["key"].asString();
    if (cmd["remove"].asBool()) {
      jsui_set_state(child, key.c_str(), nullptr);
    } else {
      std::string json = cmd["value"].toJson();
      if (cmd["patch"].asBool()) jsui_patch_state(child, key.c_str(), json.c_str());
      else jsui_set_state(child, key.c_str(), json.c_str());
    }
  }
}

// The node's content box: its layout size less padding and border.
void contentBox(const Node* node, float* x, float* y, float* w, float* h) {
  YGNodeConstRef yg = node->yoga();
  Rect r = node->layoutRect();
  float l = YGNodeLayoutGetPadding(yg, YGEdgeLeft) + YGNodeLayoutGetBorder(yg, YGEdgeLeft);
  float t = YGNodeLayoutGetPadding(yg, YGEdgeTop) + YGNodeLayoutGetBorder(yg, YGEdgeTop);
  float rr = YGNodeLayoutGetPadding(yg, YGEdgeRight) + YGNodeLayoutGetBorder(yg, YGEdgeRight);
  float b = YGNodeLayoutGetPadding(yg, YGEdgeBottom) + YGNodeLayoutGetBorder(yg, YGEdgeBottom);
  auto finite = [](float v) { return std::isfinite(v) ? v : 0.f; };
  *x = finite(l);
  *y = finite(t);
  *w = std::max(0.f, finite(r.width) - *x - finite(rr));
  *h = std::max(0.f, finite(r.height) - *y - finite(b));
}

}  // namespace

NodeState::~NodeState() {
  *life = nullptr;
  if (cache) {
    destroyChild(this);
    cache->removeView(node);
  }
}

NodeState* ensure(Node* node) {
  if (NodeState* st = stateOf(node)) return st;
  Document* doc = node->document();
  if (!doc || !doc->containers) return nullptr;
  auto st = std::make_unique<NodeState>();
  st->node = node;
  st->cache = doc->containers;
  st->cache->addView(node);
  NodeState* raw = st.get();
  node->imageCache = std::move(st);
  return raw;
}

namespace {

Services::Mode modeOf(const Value& v) {
  const std::string& m = v.asString();
  return m == "none" ? Services::None : m == "js" ? Services::Js : Services::Inherit;
}

// `*` matches any run of characters.
bool glob(const char* p, const char* s) {
  for (; *p; p++, s++) {
    if (*p == '*') {
      while (p[1] == '*') p++;
      if (!p[1]) return true;
      for (; *s; s++)
        if (glob(p + 1, s)) return true;
      return false;
    }
    if (*s != *p) return false;
  }
  return !*s;
}

}  // namespace

// The `services` prop, as the JSUIContainer component sends it: {load,
// audio, video: 'inherit' | 'none' | 'js', fonts, textures: bool,
// textureAliases: {key: parentKey}, rewrite: [[from, to]], allow, deny:
// [glob]}. Missing: inherit.
Services Services::parse(const Value& v) {
  Services s;
  s.load = modeOf(v["load"]);
  s.audio = modeOf(v["audio"]);
  s.video = modeOf(v["video"]);
  s.fonts = v["fonts"].asBool(true);
  s.textures = v["textures"].asBool(true);
  for (const auto& [k, pk] : v["textureAliases"].asObject())
    if (pk.isString()) s.textureAliases[k] = pk.asString();
  for (const Value& r : v["rewrite"].asArray())
    if (r.isArray() && r.asArray().size() == 2) s.rewrite.emplace_back(r.asArray()[0].asString(), r.asArray()[1].asString());
  for (const Value& g : v["allow"].asArray()) s.allow.push_back(g.asString());
  for (const Value& g : v["deny"].asArray()) s.deny.push_back(g.asString());
  return s;
}

bool Services::admit(std::string& url) const {
  for (const auto& [from, to] : rewrite)
    if (!from.empty() && url.compare(0, from.size(), from) == 0) {
      url = to + url.substr(from.size());
      break;
    }
  for (const std::string& g : deny)
    if (glob(g.c_str(), url.c_str())) return false;
  if (allow.empty()) return true;
  for (const std::string& g : allow)
    if (glob(g.c_str(), url.c_str())) return true;
  return false;
}

void propsChanged(Node* node, const Value& old) {
  NodeState* st = ensure(node);
  if (!st) return;
  const Value& props = node->props();
  // A new source (inline data may also come as a `load` command).
  if (!old["bundleSrc"].equals(props["bundleSrc"]) || !old["bundleData"].equals(props["bundleData"])) {
    std::string src = props["bundleSrc"].asString();
    st->src = src.empty() ? src : ResourceLoader::resolve(st->cache->runtime().baseUrl, src);
    st->data = props["bundleData"].asString();
    st->stale = true;
  }
  if (!old["services"].equals(props["services"])) {
    st->services = Services::parse(props["services"]);
    applyInheritance(st);
  }
  std::string appKey = props["appKey"].asString();
  std::string propsJson = props["initialProps"].isObject() ? props["initialProps"].toJson() : "{}";
  if (appKey != st->appKey || propsJson != st->propsJson) {
    st->appKey = std::move(appKey);
    st->propsJson = std::move(propsJson);
    st->runPending = true;
  }
  // hostState: the keys that changed become state commands (null removes).
  const Value& before = old["hostState"];
  const Value& now = props["hostState"];
  for (const auto& [key, value] : now.asObject()) {
    if (before.has(key) && before[key].equals(value)) continue;
    Value::Object c{{"op", Value("state")}, {"key", Value(key)}, {"value", value}};
    if (value.isNull()) c.emplace("remove", Value(true));
    st->commands.push_back(Value(std::move(c)));
  }
  for (const auto& [key, value] : before.asObject())
    if (!now.has(key))
      st->commands.push_back(Value(Value::Object{{"op", Value("state")}, {"key", Value(key)}, {"remove", Value(true)}}));
}

bool command(Node* node, const Value& cmd) {
  NodeState* st = node && node->type() == NodeType::Container ? ensure(node) : nullptr;
  if (!st) return false;
  const std::string& op = cmd["op"].asString();
  if (op == "load") {
    // Inline bundle source (the React component sends bundleData this way,
    // not as a prop the bridge would copy on every update).
    st->src.clear();
    st->data = cmd["data"].asString();
    st->stale = true;
    return true;
  }
  if (op != "post" && op != "call" && op != "resolve" && op != "reject" && op != "state" && op != "reload")
    return false;
  st->commands.push_back(cmd);
  return true;
}

JsuiRuntime* child(const Node* node) {
  NodeState* st = stateOf(node);
  return st ? st->child : nullptr;
}

bool toChild(const Node* node, float localX, float localY, float* cx, float* cy) {
  if (!child(node)) return false;
  float x, y, w, h;
  contentBox(node, &x, &y, &w, &h);
  *cx = localX - x;
  *cy = localY - y;
  return true;
}

void paint(Node* node, SkCanvas* canvas, const SkRect& content, const SkRRect& clip) {
  NodeState* st = stateOf(node);
  if (!st || !st->child) return;
  Surface& s = internalRuntime(st->child)->main();
  float pr = s.doc.pixelRatio();
  int dw = int(std::ceil(st->width * pr)), dh = int(std::ceil(st->height * pr));
  if (dw <= 0 || dh <= 0 || !(pr > 0)) return;
  NestedPaintScope nested;
  canvas->save();
  canvas->clipRRect(clip, true);
  canvas->clipRect(content, true);
  canvas->translate(content.x(), content.y());
  // The child's paint scales by its pixel ratio (the parent's): the canvas
  // is in the parent's logical px here.
  canvas->scale(1 / pr, 1 / pr);
  s.renderCanvas(canvas, dw, dh, false);
  canvas->restore();
}

}  // namespace container

// ------------------------------------------------------------- Containers

Containers::Containers(Runtime& rt) : rt_(rt) {}

Containers::~Containers() {
  // Nodes outliving the cache (none in practice: the surfaces go first) no
  // longer reach it.
  for (Node* n : views_)
    if (auto* st = static_cast<container::NodeState*>(n->imageCache.get())) {
      container::destroyChild(st);
      st->cache = nullptr;
    }
}

void Containers::addView(Node* node) { views_.insert(node); }
void Containers::removeView(Node* node) { views_.erase(node); }

void Containers::tick(double nowMs) {
  // Callbacks only queue events (delivered after this): the set is stable.
  std::vector<Node*> list(views_.begin(), views_.end());
  for (Node* n : list) {
    auto* st = static_cast<container::NodeState*>(n->imageCache.get());
    if (!st) continue;
    float x, y, w, h;
    container::contentBox(n, &x, &y, &w, &h);
    Document* doc = n->document();
    float pr = doc ? doc->pixelRatio() : 1;
    bool resized = w != st->width || h != st->height || pr != st->pixelRatio;
    st->width = w;
    st->height = h;
    st->pixelRatio = pr;
    if (st->stale) {
      st->stale = false;
      container::createChild(st);
      resized = false;
      n->markPaintDirty();
    }
    if (!st->child) {
      // Commands without a child: calls fail, the rest has nothing to reach.
      std::vector<Value> cmds = std::move(st->commands);
      st->commands.clear();
      for (const Value& c : cmds)
        if (c["op"].asString() == "call" || c["op"].asString() == "reload") container::runCommand(st, c);
      continue;
    }
    Runtime* childRt = internalRuntime(st->child);
    if (resized) childRt->main().setViewport(w, h, pr);
    // Fonts registered on the parent since: the child's text re-shapes too.
    if (st->parentFonts != rt_.fonts.generation) {
      st->parentFonts = rt_.fonts.generation;
      childRt->fonts.changed();
      childRt->fontsChanged();
    }
    if (st->runPending) {
      st->runPending = false;
      jsui_surface_run(st->child, st->appKey.c_str(), st->propsJson.c_str());
    }
    std::vector<Value> cmds = std::move(st->commands);
    st->commands.clear();
    for (const Value& c : cmds) {
      container::runCommand(st, c);
      if (!st->child) break;  // a reload of a source that is gone
    }
    if (!st->child) continue;
    jsui_tick(st->child, nowMs);
    if (jsui_needs_redraw(st->child)) n->markPaintDirty();
  }
}

void Containers::tickNative(double nowMs) {
  for (Node* n : views_) {
    JsuiRuntime* child = container::child(n);
    if (!child) continue;
    jsui_tick_native(child, nowMs);
    if (jsui_needs_redraw(child)) n->markPaintDirty();
  }
}

bool Containers::animating() const {
  if (!events.empty()) return true;
  for (Node* n : views_) {
    auto* st = static_cast<const container::NodeState*>(n->imageCache.get());
    if (!st) continue;
    if (st->stale || st->runPending || !st->commands.empty()) return true;
    if (st->child && (jsui_wants_frame(st->child) || jsui_pending_loads(st->child) > 0)) return true;
  }
  return false;
}

double Containers::nextTimer() const {
  double due = std::numeric_limits<double>::infinity();
  for (Node* n : views_)
    if (JsuiRuntime* child = container::child(n)) due = std::min(due, jsui_next_timer(child));
  return due;
}

}  // namespace jsui
