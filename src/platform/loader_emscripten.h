// Browser backend of the built-in ResourceLoader (Emscripten builds only).
//
// Requests go through the page's fetch(); the response bytes are copied into
// the WASM heap by JS and handed back to ResourceLoader::complete(), so they
// are delivered on the runtime thread by the next pump() like every other
// backend. Relative URLs are resolved by fetch() against the document, but the
// web host passes an absolute bundle URL, so the runtime's own resolve() has
// already made them absolute against the bundle.
#pragma once

#include <cstdint>
#include <string>

namespace jsui {

class ResourceLoader;
struct LoadRequest;

#ifdef __EMSCRIPTEN__
// Starts a fetch (method, headers, body, timeout honoured). Returns false for
// URLs this backend does not handle (data: URIs, decoded generically).
bool emscriptenLoad(ResourceLoader* loader, uint64_t id, const LoadRequest& req);
// Aborts in-flight fetches of `loader` and guarantees no completion reaches it
// afterwards. Called from ~ResourceLoader.
void emscriptenLoaderDestroyed(ResourceLoader* loader);
#endif

}  // namespace jsui
