#include "js/bytecode.h"

#include <quickjs.h>

#include <cstdint>
#include <cstring>

#ifndef JSUI_BYTECODE_KEY
#define JSUI_BYTECODE_KEY "unknown"
#endif

namespace jsui {

namespace {

constexpr char kMagic[8] = {'\0', 'J', 'S', 'U', 'I', 'B', 'C', '\1'};

void putU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back(char((v >> (8 * i)) & 0xff));
}

bool getU32(std::string_view& in, uint32_t& v) {
  if (in.size() < 4) return false;
  v = 0;
  for (int i = 0; i < 4; i++) v |= uint32_t(uint8_t(in[i])) << (8 * i);
  in.remove_prefix(4);
  return true;
}

bool getBlock(std::string_view& in, std::string_view& block) {
  uint32_t n = 0;
  if (!getU32(in, n) || in.size() < n) return false;
  block = in.substr(0, n);
  in.remove_prefix(n);
  return true;
}

}  // namespace

const char* bytecodeKey() {
  static const std::string key = std::string("quickjs-ng ") + JS_GetVersion() + " " + JSUI_BYTECODE_KEY;
  return key.c_str();
}

bool isBundleContainer(std::string_view data) {
  return data.size() >= sizeof(kMagic) && std::memcmp(data.data(), kMagic, sizeof(kMagic)) == 0;
}

bool parseBundleContainer(std::string_view data, BundleParts& parts) {
  if (!isBundleContainer(data)) return false;
  data.remove_prefix(sizeof(kMagic));
  if (!getBlock(data, parts.key) || !getBlock(data, parts.bytecode)) return false;
  parts.source = data;
  return true;
}

bool compileBundle(std::string_view source, const std::string& name, bool keepSource, std::string& out,
                   std::string& error) {
  JSRuntime* rt = JS_NewRuntime();
  JSContext* ctx = rt ? JS_NewContext(rt) : nullptr;
  if (!ctx) {
    if (rt) JS_FreeRuntime(rt);
    error = "out of memory";
    return false;
  }
  // JS_Eval needs a NUL-terminated buffer.
  std::string src(source);
  JSValue fn = JS_Eval(ctx, src.c_str(), src.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  bool ok = !JS_IsException(fn);
  if (ok) {
    size_t size = 0;
    // Line numbers stay (stack traces); function source text does not.
    uint8_t* bc = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE);
    if (bc) {
      const char* key = bytecodeKey();
      out.clear();
      out.reserve(sizeof(kMagic) + 8 + std::strlen(key) + size + (keepSource ? source.size() : 0));
      out.append(kMagic, sizeof(kMagic));
      putU32(out, uint32_t(std::strlen(key)));
      out.append(key);
      putU32(out, uint32_t(size));
      out.append(reinterpret_cast<const char*>(bc), size);
      if (keepSource) out.append(source);
      js_free(ctx, bc);
    } else {
      ok = false;
    }
  }
  if (!ok) {
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    error = msg ? msg : "compilation failed";
    if (msg) JS_FreeCString(ctx, msg);
    if (JS_IsError(exc)) {
      JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
      const char* s = JS_IsUndefined(stack) ? nullptr : JS_ToCString(ctx, stack);
      if (s && *s) error += std::string("\n") + s;
      if (s) JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
  }
  JS_FreeValue(ctx, fn);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return ok;
}

}  // namespace jsui
