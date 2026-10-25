// __jsui_native members for game data (docs/ARCHITECTURE.md "Game data"):
// shared host state and request / response calls in both directions.
//
//   getState(key) -> json string | undefined     stateVersion(key) -> number (0: unset)
//   setState(key, json | undefined) -> version   patchState(key, json) -> version
//   stateKeys() -> [key]                         statePatches(key, since) -> [patch json] | undefined
//   hostCall(name, json, settle(ok, payload)) -> id (0: the host takes no calls)
//   cancelCall(id) -> bool                       callResult(id, ok, payload)
//   buffer(key) -> [ArrayBuffer, type, count, version] | undefined
//   bufferVersion(key) -> number (0: unset)      bufferKeys() -> [key]
//   texture(key) -> [width, height, version] | undefined   textureKeys() -> [key]
//
// The stores are shared with the owner thread (a JS thread reads and writes
// them while the embedder does): HostState and the texture size table lock;
// buffers are read through published copies in JS-thread mode; the
// embedder's on_state / on_call / on_call_cancel are queued for the tick
// there (Runtime::toHost).
#include <string>

#include "dev_session.h"
#include "js/engine_impl.h"
#include "runtime.h"

namespace jsui {

namespace {

JSValue throwSyntax(JSContext* ctx, const std::string& what, const std::string& error) {
  return JS_ThrowSyntaxError(ctx, "%s: invalid JSON (%s)", what.c_str(), error.c_str());
}

uint64_t toId(JSContext* ctx, JSValueConst v) {
  double d = 0;
  if (JS_ToFloat64(ctx, &d, v) < 0) return 0;
  return d >= 1 && d < 9007199254740992.0 ? uint64_t(d) : 0;
}

JSValue js_getState(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_UNDEFINED;
  HostState& state = runtimeOf(ctx).state;
  auto lock = state.lock();
  const std::string* json = state.json(toStdString(ctx, argv[0]));
  return json ? JS_NewStringLen(ctx, json->data(), json->size()) : JS_UNDEFINED;
}

// statePatches(key, sinceVersion) -> [patch json] | undefined: the merge
// patches since a version the caller has parsed (see HostState::patchesSince).
JSValue js_statePatches(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_UNDEFINED;
  double since = 0;
  if (JS_ToFloat64(ctx, &since, argv[1]) < 0) return JS_EXCEPTION;
  if (!(since >= 1 && since < 9007199254740992.0)) return JS_UNDEFINED;
  std::vector<const std::string*> patches;
  HostState& state = runtimeOf(ctx).state;
  auto lock = state.lock();
  if (!state.patchesSince(toStdString(ctx, argv[0]), uint64_t(since), &patches)) return JS_UNDEFINED;
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < patches.size(); i++)
    JS_SetPropertyUint32(ctx, arr, uint32_t(i), JS_NewStringLen(ctx, patches[i]->data(), patches[i]->size()));
  return arr;
}

JSValue js_stateVersion(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_NewInt32(ctx, 0);
  return JS_NewFloat64(ctx, double(runtimeOf(ctx).state.version(toStdString(ctx, argv[0]))));
}

JSValue js_stateKeys(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  auto keys = runtimeOf(ctx).state.keys();
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < keys.size(); i++)
    JS_SetPropertyUint32(ctx, arr, uint32_t(i), JS_NewStringLen(ctx, keys[i].data(), keys[i].size()));
  return arr;
}

// magic 0: setState(key, json | undefined), 1: patchState(key, json)
JSValue js_writeState(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
  Runtime& rt = runtimeOf(ctx);
  if (argc < 1) return JS_ThrowTypeError(ctx, "%s(key, json): key is required", magic ? "patchState" : "setState");
  std::string key = toStdString(ctx, argv[0]);
  bool remove = argc < 2 || JS_IsUndefined(argv[1]);
  bool changed = false;
  if (remove) {
    changed = rt.state.remove(key, true);
  } else {
    std::string json = toStdString(ctx, argv[1]);
    std::string error;
    HostState::Result r = magic ? rt.state.patch(key, json, &error, true) : rt.state.set(key, json, &error, true);
    if (r == HostState::Result::Invalid) return throwSyntax(ctx, magic ? "patchState" : "setState", error);
    changed = r == HostState::Result::Changed;
  }
  // JS notifies its own subscribers (the key is not queued for JS); the
  // embedder is told here (with the tree JS built so far applied: on_state
  // may look at it). JS-thread mode: in the tick, with the text JS left;
  // native Animated host-state bindings follow the key there too
  // (HostState::deferUiChanges).
  if (changed && implOf(ctx).threaded) {
    std::string json;
    bool set = rt.state.jsonCopy(key, &json);
    Runtime* r = &rt;
    rt.hostQueue.post([r, key, json = std::move(json), set] {
      if (r->state.onChange) r->state.onChange(key);
      r->uiStateChanged(key, set ? &json : nullptr);
    });
  } else if (changed) {
    implOf(ctx).syncPoint();
    rt.uiStateChanged(key);
  }
  return JS_NewFloat64(ctx, double(rt.state.version(key)));
}

// Backing store of an ArrayBuffer over a published buffer copy: the
// ArrayBuffer holds a reference to the copy (dropped when it is freed).
void* copyBuffer(JSRuntime*, void* opaque, void*, size_t size) {
  if (size == 0) delete static_cast<std::shared_ptr<const HostBuffers::Copy>*>(opaque);
  return nullptr;  // never resized
}

// JS-thread mode: buffer(key) over the copy the owner thread published
// (HostBuffers::publish). A key read for the first time is subscribed and
// copied at once (a proxy call), then once per tick while it changes. A new
// copy is a new ArrayBuffer; one JS still holds keeps its contents.
JSValue publishedBuffer(JSContext* ctx, JsEngine::Impl& impl, const std::string& key) {
  HostBuffers& buffers = impl.rt.buffers;
  std::shared_ptr<const HostBuffers::Copy> copy = buffers.published(key);
  if (!copy && buffers.publishedVersion(key)) {
    buffers.subscribe(key);
    impl.rt.hostQueue.call([&] { buffers.publish(); }, Runtime::kProxyTimeoutMs);
    copy = buffers.published(key);
  }
  if (!copy) return JS_UNDEFINED;
  auto it = impl.bufferViews.find(key);
  if (it == impl.bufferViews.end() || it->second.data != copy.get()) {
    if (it != impl.bufferViews.end()) {
      JS_FreeValue(ctx, it->second.buffer);
      impl.bufferViews.erase(it);
    }
    JSValue ab;
    if (copy->bytes.empty()) {
      ab = JS_NewArrayBufferCopy(ctx, nullptr, 0);
    } else {
      auto* ref = new std::shared_ptr<const HostBuffers::Copy>(copy);
      ab = JS_NewArrayBuffer(ctx, const_cast<uint8_t*>(copy->bytes.data()), copy->bytes.size(), 0, copyBuffer, ref,
                             false);
    }
    if (JS_IsException(ab)) return ab;
    it = impl.bufferViews.emplace(key, JsEngine::Impl::BufferView{ab, copy.get(), copy->bytes.size()}).first;
  }
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, it->second.buffer));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, int32_t(copy->type)));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, double(copy->count)));
  JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, double(copy->version)));
  return arr;
}

// Frame data. The ArrayBuffer spans the buffer's storage (its capacity: JS
// views the first `count` elements); the same object comes back while the
// storage stays put, and it is detached before the storage is freed.
JSValue js_buffer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_UNDEFINED;
  auto& impl = implOf(ctx);
  std::string key = toStdString(ctx, argv[0]);
  if (impl.threaded) return publishedBuffer(ctx, impl, key);
  const HostBuffers::Entry* e = impl.rt.buffers.find(key);
  if (!e) return JS_UNDEFINED;
  auto it = impl.bufferViews.find(key);
  if (it == impl.bufferViews.end() || it->second.data != e->data() || it->second.bytes != e->capacity) {
    if (it != impl.bufferViews.end()) impl.releaseBuffer(key);
    JSValue ab = JS_NewArrayBuffer(ctx, e->data(), e->capacity, 0, nullptr, nullptr, false);
    if (JS_IsException(ab)) return ab;
    it = impl.bufferViews.emplace(key, JsEngine::Impl::BufferView{ab, e->data(), e->capacity}).first;
  }
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, it->second.buffer));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, int32_t(e->type)));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, double(e->count)));
  JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, double(e->version)));
  return arr;
}

JSValue js_bufferVersion(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_NewInt32(ctx, 0);
  const HostBuffers& b = runtimeOf(ctx).buffers;
  std::string key = toStdString(ctx, argv[0]);
  return JS_NewFloat64(ctx, double(implOf(ctx).threaded ? b.publishedVersion(key) : b.version(key)));
}

JSValue js_bufferKeys(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  const HostBuffers& b = runtimeOf(ctx).buffers;
  auto keys = implOf(ctx).threaded ? b.publishedKeys() : b.keys();
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < keys.size(); i++)
    JS_SetPropertyUint32(ctx, arr, uint32_t(i), JS_NewStringLen(ctx, keys[i].data(), keys[i].size()));
  return arr;
}

// Host textures: what JS needs to lay views out (useHostTexture); the pixels
// never reach JS.
JSValue js_texture(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_UNDEFINED;
  int width = 0, height = 0;
  uint64_t version = 0;
  if (!runtimeOf(ctx).textures.size(toStdString(ctx, argv[0]), &width, &height, &version)) return JS_UNDEFINED;
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, width));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, height));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, double(version)));
  return arr;
}

JSValue js_textureKeys(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  auto keys = runtimeOf(ctx).textures.keys();
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < keys.size(); i++)
    JS_SetPropertyUint32(ctx, arr, uint32_t(i), JS_NewStringLen(ctx, keys[i].data(), keys[i].size()));
  return arr;
}

JSValue js_hostCall(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto& impl = implOf(ctx);
  Runtime& rt = impl.rt;
  if (argc < 3 || !JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "hostCall(name, json, settle)");
  if (!rt.config.on_call && !(rt.session && rt.session->joined())) return JS_NewInt32(ctx, 0);
  std::string name = toStdString(ctx, argv[0]);
  std::string json = JS_IsUndefined(argv[1]) ? "null" : toStdString(ctx, argv[1]);
  if (rt.session && rt.session->joined())
    rt.session->uiEvent("call", {{"name", Value(name)}, {"args", Value::parseJson(json)}});
  if (!rt.config.on_call) return JS_NewInt32(ctx, 0);
  uint64_t id = rt.newCallId();
  // Registered before on_call runs, so the embedder may answer from inside it.
  impl.hostCalls[id] = JS_DupValue(ctx, argv[2]);
  if (!impl.threaded) impl.syncPoint();
  Runtime* r = &rt;
  rt.toHost([r, id, name = std::move(name), json = std::move(json)] {
    r->config.on_call(r->config.call_user, id, name.c_str(), json.c_str());
  });
  return JS_NewFloat64(ctx, double(id));
}

JSValue js_cancelCall(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Runtime& rt = runtimeOf(ctx);
  uint64_t id = argc > 0 ? toId(ctx, argv[0]) : 0;
  if (!id || !rt.js->dropCall(id)) return JS_FALSE;
  Runtime* r = &rt;
  rt.toHost([r, id] { r->hostCallCancelled(id); });
  return JS_TRUE;
}

JSValue js_callResult(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  uint64_t id = argc > 0 ? toId(ctx, argv[0]) : 0;
  if (!id) return JS_UNDEFINED;
  bool ok = argc > 1 && JS_ToBool(ctx, argv[1]);
  std::string payload = argc > 2 && !JS_IsUndefined(argv[2]) ? toStdString(ctx, argv[2]) : (ok ? "null" : "");
  Runtime* r = &runtimeOf(ctx);
  r->toHost([r, id, ok, payload = std::move(payload)]() mutable { r->uiCallSettled(id, ok, std::move(payload)); });
  return JS_UNDEFINED;
}

}  // namespace

void installHostDataBridge(JsEngine::Impl& impl, JSValue native) {
  JSContext* ctx = impl.ctx;
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int len;
  } fns[] = {{"getState", js_getState, 1},   {"stateVersion", js_stateVersion, 1}, {"stateKeys", js_stateKeys, 0},
             {"statePatches", js_statePatches, 2},
             {"buffer", js_buffer, 1},       {"bufferVersion", js_bufferVersion, 1}, {"bufferKeys", js_bufferKeys, 0},
             {"texture", js_texture, 1},     {"textureKeys", js_textureKeys, 0},
             {"hostCall", js_hostCall, 3},   {"cancelCall", js_cancelCall, 1},     {"callResult", js_callResult, 3}};
  for (auto& f : fns) JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx, native, "setState",
                    JS_NewCFunctionMagic(ctx, js_writeState, "setState", 2, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, native, "patchState",
                    JS_NewCFunctionMagic(ctx, js_writeState, "patchState", 2, JS_CFUNC_generic_magic, 1));
}

}  // namespace jsui
