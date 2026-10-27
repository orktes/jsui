# Networking: `fetch()` on every host

A JSUI bundle uses the web's `fetch()`:

```js
const res = await fetch('/api/scores?top=10');   // resolves against the bundle's URL
const scores = await res.json();

// Streamed: pieces arrive as the server sends them.
const reader = (await fetch('https://example.com/feed')).body
  .pipeThrough(new TextDecoderStream()).getReader();
for (;;) {
  const { value, done } = await reader.read();
  if (done) break;
  appendLine(value);
}

// Forms and files.
const form = new FormData();
form.append('name', 'Player 1');
form.append('avatar', new File([pngBytes], 'avatar.png', { type: 'image/png' }));
await fetch('https://example.com/upload', { method: 'POST', body: form });
```

The same code runs on every host: the browser, the native viewer and
example engines (raylib, SDL3, sokol), Godot, Unity, Unreal, iOS and
Android. What differs is who does the network I/O underneath, and that is
the host's choice (see [Host networking](#host-networking)).

## What JS gets

* `fetch(input, init)`: `method`, `headers`, `body`, `signal`
  (`AbortController`), `redirect` (`'follow'`, `'error'`, `'manual'`) and a
  JSUI extension, `timeout` (ms, the whole request). URLs resolve against the
  bundle's URL (`/api` on a bundle served from `http://localhost:5173/` is
  `http://localhost:5173/api`). `fetch()` resolves once the status and
  headers are in; the body streams after that. Network errors reject with a
  `TypeError`, HTTP errors resolve with `ok === false`, an abort before the
  headers rejects with the signal's reason (an `AbortError`) and an abort
  after them errors the body.
* `Response`: `status`, `ok`, `statusText`, `url`, `redirected`, `headers`,
  `body` (a `ReadableStream` of `Uint8Array`s), `bodyUsed`, `text()`,
  `json()`, `arrayBuffer()`, `bytes()`, `blob()`, `formData()`, `clone()`
  (tees a streamed body), `Response.json()` / `error()` / `redirect()`.
* Request bodies: a string, `ArrayBuffer` / typed array / `DataView`,
  `URLSearchParams`, `Blob` / `File`, `FormData` (sent as
  `multipart/form-data`) or a `ReadableStream` (read to the end first: uploads
  are not streamed).
* `Headers`, `Request` (`clone()`, made from another `Request`).
* Streams: `ReadableStream` (default readers, `for await`, `tee()`,
  `pipeTo()`, `pipeThrough()`, `ReadableStream.from()`), `WritableStream`,
  `TransformStream`, `TextDecoderStream`, `TextEncoderStream`,
  `CountQueuingStrategy`, `ByteLengthQueuingStrategy`. No byte streams (BYOB
  readers).
* `Blob` (`size`, `type`, `slice()`, `text()`, `arrayBuffer()`, `bytes()`,
  `stream()`), `File` (`name`, `lastModified`), `FormData` (the whole API;
  `res.formData()` parses `multipart/form-data` and
  `application/x-www-form-urlencoded`).

Not there: cookies (a host stack may keep its own), `credentials` / `mode` /
`cache` are accepted and ignored, there is no CORS (native hosts are not a
browser; the web build is the browser's own fetch), `URL.createObjectURL`,
and streamed uploads. A streamed body has no backpressure into the host:
pieces queue in JS until read, like a fast server with a slow reader in a
browser.

## Who does the I/O

| Host | Default | https:// |
|------|---------|----------|
| Web (`@orktes/jsui-web`) | the page's `fetch()`, or `createJsui({ fetch })` | yes |
| macOS / iOS (viewer, engine examples, SwiftUI) | built-in loader: own HTTP/1.1 client for `http://`, `NSURLSession` for `https://` | yes, system trust store |
| Windows | built-in loader: own client for `http://`, WinHTTP for `https://` | yes, system trust store |
| Linux | built-in loader: own client; TLS through the system's OpenSSL (`libssl.so.3`, else `.so.1.1`), loaded on first use | yes, when OpenSSL is installed |
| Android (example host) | the host's `on_request`: `HttpURLConnection` over JNI | yes |
| Godot | the plugin's `on_request`: Godot's `HTTPClient` (`use_engine_http`, default on) | yes (Godot's TLS) |
| Unity | the plugin's `on_request`: `UnityWebRequest` (`useEngineHttp`, default on) | yes |
| Unreal | the plugin's `on_request`: the `HTTP` module (`FHttpModule`) | yes |

No TLS library is compiled into JSUI. The OS (or the engine) already has
one, it keeps its certificates up to date, and it knows the proxy settings;
so `https://` costs libjsui only the glue (a few KB) instead of the
~300-600 KB a bundled TLS stack (mbedTLS, BoringSSL) would add per
architecture. On Linux the library is loaded with `dlopen` only when the
first `https://` request is made, so a game that never makes one does not
need OpenSSL.

The built-in client sends `Accept-Encoding: identity`, follows up to 20
redirects (303, and 301 / 302 after a POST, switch to GET like browsers),
and has no overall timeout by default: 30 s to the response headers, then 30 s
of silence between body pieces. A `timeout` in `fetch()` bounds the whole
request.

## Host networking

The host decides how loads happen with `JsuiConfig.on_request`
(`JsuiRequestFn`). Every load the runtime makes reaches it: `fetch()`, the
bundle, images, fonts, sounds, Lottie files. It gets the whole request
(`JsuiRequest`: URL, method, headers, body, timeout, redirect mode, and
whether JS streams the body) and either takes it (returns 0) or declines it
(nonzero), and a declined request goes on to `JsuiConfig.load` (the older
URL-only loader) or the built-in loader. So a game can serve `pak://` or
`res://` itself and leave the network to JSUI, or take the network and leave
files to JSUI, or anything in between.

A request it takes is answered with:

```c
jsui_request_respond(rt, id, status, status_text, final_url, headers, header_count);
jsui_request_body(rt, id, bytes, size);   /* any number of times */
jsui_request_end(rt, id, NULL);           /* or an error message: the body failed part way */
```

or all at once with `jsui_resource_complete(rt, id, status, content_type,
data, size)`. Status 0 (with the reason in `status_text`) is a network
error. These functions are thread safe, so a network thread can call them
directly; the runtime delivers the pieces on its own thread during
`jsui_tick()`, in order. When JS no longer wants a request (an abort, a
reload, the runtime going away) `on_request_cancel` is called on the owner
thread; answering a cancelled request is harmless.

```c
static int on_request(void* user, JsuiRuntime* rt, uint64_t id, const JsuiRequest* req) {
  if (strncmp(req->url, "pak://", 6) == 0) {
    Blob b = pak_read(req->url + 6);
    jsui_resource_complete(rt, id, b.data ? 200 : 404, NULL, b.data, b.size);
    return 0;
  }
  return 1;  /* everything else: the built-in loader */
}

cfg.on_request = on_request;
cfg.on_request_cancel = on_cancel;   /* optional */
cfg.request_user = game;
```

On the web the page is the host: `createJsui({ fetch: (url, init) => ... })`
takes a function shaped like `fetch` (it gets the absolute URL and a
`RequestInit`, and returns a `Response`) that then does every network load
the runtime makes, streamed bodies included (page mode; worker mode falls
back to the page's thread when it is set).

A `<JSUIContainer>` child loads through its parent (and the parent's
`on_request`), streamed bodies included, unless its embedder gave it its own
`on_request` or `load`.

### Engine plugins

The engine plugins answer network URLs with the engine's own HTTP stack and
decline the rest, so a game's networking settings (proxies, certificates,
platform rules on consoles) apply to the UI too:

* **Godot** (`JsuiView`): `HTTPClient`, polled on the view's process step;
  `res://` and `user://` stay with Godot's `FileAccess`. A bundle loaded from
  `res://` reaches the network this way. `use_engine_http = false` hands
  network URLs to the built-in loader instead.
* **Unity** (`JsuiView`): `UnityWebRequest` with a streaming download
  handler (the package depends on Unity's web request module);
  `useEngineHttp = false` turns it off.
* **Unreal**: `FHttpModule` requests, streamed piece by piece on UE 5.5 and
  newer; `bUseEngineHttp = false` turns it off. Unreal always follows
  redirects itself, so requests with `redirect: 'error'` or `'manual'` go to
  the built-in loader.

Engine stacks give no reason phrase; the runtime fills in the standard one
(`404` → `Not Found`) whenever a host answers without a status text.

`redirect: 'manual'` hands JS the real 3xx response (status, `Location`)
rather than the browser's opaque `opaqueredirect` with status 0: a native UI
has no page to navigate, so the redirect itself is the useful part.
